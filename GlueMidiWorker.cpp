#include "GlueMidiWorker.h"
#include <utility> //std::move to store the log cb in the ctor

static void MidiInputCallback(
	double DeltaTime,
	std::vector<unsigned char>* Message,
	void* UserData)
{
	if (Message == nullptr || Message->empty() || UserData == nullptr)
	{
		return;
	}

	auto* Input = static_cast<InputItem*>(UserData);

	if (!Input->AcceptCallbacks_atomic.load(std::memory_order_acquire))
	{
		return;
	}

	GlueMidiWorker* Worker = Input->Worker;

	if (Worker == nullptr)
	{
		return;
	}

	const bool ShouldRoute = !Input->Muted_atomic.load(std::memory_order_relaxed);

	Worker->EnqueueMidiMessage(Input->Id, DeltaTime, ShouldRoute, *Message);
}

GlueMidiWorker::GlueMidiWorker(LogCallback InLogCallback)
	: LogCallback_(std::move(InLogCallback))
{
}

GlueMidiWorker::~GlueMidiWorker()
{
	Stop();
}

void GlueMidiWorker::Start()
{
	if (MidiThreadRunning_.exchange(true, std::memory_order_acq_rel))
	{
		return;
	}

	MidiThread_ = std::thread(&GlueMidiWorker::ThreadMain, this);
}

void GlueMidiWorker::Stop()
{
	if (!MidiThread_.joinable())
	{
		return;
	}

	MidiCommand Command;
	Command.Type = EMidiCommandType::Shutdown;
	QueueCommand(std::move(Command));

	MidiThread_.join();
}

void GlueMidiWorker::QueueOpenOutput(int OutputIndex)
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::OpenOutput;
	Command.OutputIndex = OutputIndex;

	QueueCommand(std::move(Command));
}

void GlueMidiWorker::QueueCloseOutput()
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::CloseOutput;

	QueueCommand(std::move(Command));
}

void GlueMidiWorker::QueueOpenInput(int InputIndex)
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::OpenInput;
	Command.InputIndex = InputIndex;

	QueueCommand(std::move(Command));
}

void GlueMidiWorker::QueueCloseInput(int InputIndex)
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::CloseInput;
	Command.InputIndex = InputIndex;

	QueueCommand(std::move(Command));
}

void GlueMidiWorker::QueueCheckPorts()
{
	{
		std::lock_guard<std::mutex> Lock(CommandMutex_);

		// suppress dupe checks
		for (const MidiCommand& Existing : CommandQueue_)
		{
			if (Existing.Type == EMidiCommandType::CheckPorts || Existing.Type == EMidiCommandType::RefreshPorts)
			{
				return;
			}
		}

		MidiCommand Command;
		Command.Type = EMidiCommandType::CheckPorts;
		CommandQueue_.push_back(std::move(Command));
	}

	WorkerCondition_.notify_one();
}

void GlueMidiWorker::QueueRefreshPorts()
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::RefreshPorts;

	QueueCommand(std::move(Command));
}

void GlueMidiWorker::QueueSetInputMuted(int InputIndex, bool Muted)
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::SetInputMuted;
	Command.InputIndex = InputIndex;
	Command.BoolValue = Muted;

	QueueCommand(std::move(Command));
}

void GlueMidiWorker::QueueReleaseAll()
{
	MidiCommand Command;
	Command.Type = EMidiCommandType::ReleaseAll;

	QueueCommand(std::move(Command));
}

MidiPortUiSnapshot GlueMidiWorker::GetSnapshot() const
{
	std::lock_guard<std::mutex> Lock(SnapshotMutex_);
	return Snapshot_;
}

std::vector<QueuedMidiMessage> GlueMidiWorker::DrainMonitorMessages()
{
	std::deque<QueuedMidiMessage> LocalQueue;

	{
		std::lock_guard<std::mutex> Lock(MonitorMidiMutex_);
		LocalQueue.swap(MonitorMidiQueue_);
	}

	std::vector<QueuedMidiMessage> Result;
	Result.reserve(LocalQueue.size());

	while (!LocalQueue.empty())
	{
		Result.push_back(std::move(LocalQueue.front()));
		LocalQueue.pop_front();
	}

	return Result;
}

void GlueMidiWorker::EnqueueMidiMessage(uint64_t InputId, double DeltaTime, bool ShouldRoute, const std::vector<unsigned char>& Message)
{
	QueuedMidiMessage Queued;
	Queued.InputId = InputId;
	Queued.DeltaTime = DeltaTime;
	Queued.ShouldRoute = ShouldRoute;
	Queued.Data = Message;

	{
		std::lock_guard<std::mutex> Lock(IncomingMidiMutex_);

		// prevent choking 
		if (IncomingMidiQueue_.size() >= MaxIncomingMidiMessages)
		{
			IncomingMidiQueue_.pop_front();
		}

		IncomingMidiQueue_.push_back(std::move(Queued));
	}

	WorkerCondition_.notify_one();
}

void GlueMidiWorker::ThreadMain()
{
	// Construct all top-level RtMidi objects on this thread
	try
	{
		MidiEnumerator_ = std::make_unique<RtMidiIn>();
		MidiEnumerator_->setBufferSize(2048, 4);

		MidiOutput_ = std::make_unique<RtMidiOut>();
	}
	catch (const RtMidiError& Error)
	{
		Error.printMessage();
		Log(Error.getMessage());

		MidiOutput_.reset();
		MidiEnumerator_.reset();

		MidiThreadRunning_.store(false, std::memory_order_release);

		return;
	}

	// run the thread loop
	while (MidiThreadRunning_.load(std::memory_order_acquire))
	{
		{
			std::unique_lock<std::mutex> Lock(IncomingMidiMutex_);

			WorkerCondition_.wait(Lock, [this]
				{
					return
						!MidiThreadRunning_.load(std::memory_order_acquire)
						|| !IncomingMidiQueue_.empty()
						|| HasPendingCommands();
				});
		}

		// Commands are handled first so changes such as output switching
		// take effect before subsequently queued MIDI is routed.
		ProcessPendingCommands();

		// Shutdown is performed by a command, which clears this flag.
		if (!MidiThreadRunning_.load(std::memory_order_acquire))
		{
			break;
		}

		// Drain every currently queued incoming MIDI message.
		while (true)
		{
			QueuedMidiMessage Message;

			{
				std::lock_guard<std::mutex> Lock(IncomingMidiMutex_);

				if (IncomingMidiQueue_.empty())
				{
					break;
				}

				Message = std::move(IncomingMidiQueue_.front());

				IncomingMidiQueue_.pop_front();
			}

			if (Message.ShouldRoute)
			{
				SendMessageOnPort(Message.Data);
			}

			// Monitoring is best-effort and must never grow without limit.
			{
				std::lock_guard<std::mutex> Lock(MonitorMidiMutex_);

				if (MonitorMidiQueue_.size() >= MaxMonitorMidiMessages)
				{
					MonitorMidiQueue_.pop_front();
				}

				MonitorMidiQueue_.push_back(std::move(Message));
			}
		}
	}

	// If we get this far, shutdown is presumably in progress and should've
	// already closed ports and disabled callbacks
	InputItems_.clear();

	MidiOutput_.reset();
	MidiEnumerator_.reset();

}

void GlueMidiWorker::QueueCommand(MidiCommand Command)
{
	{
		std::lock_guard<std::mutex> Lock(CommandMutex_);
		CommandQueue_.push_back(std::move(Command));
	}

	WorkerCondition_.notify_one();
}

bool GlueMidiWorker::HasPendingCommands()
{
	std::lock_guard<std::mutex> Lock(CommandMutex_);
	return !CommandQueue_.empty();
}

void GlueMidiWorker::ProcessPendingCommands()
{
	// broken out from GlueMidi.cpp
	while (true)
	{
		MidiCommand Command;

		{
			std::lock_guard<std::mutex> Lock(CommandMutex_);

			if (CommandQueue_.empty())
			{
				break;
			}

			Command = std::move(CommandQueue_.front());
			CommandQueue_.pop_front();
		}

		switch (Command.Type)
		{
		case EMidiCommandType::OpenOutput:
		{
			const int Index = Command.OutputIndex;

			if (!MidiOutput_ || Index < 0 || Index >= static_cast<int>(OutputNames_.size()))
			{
				break;
			}

			try
			{
				if (MidiOutput_->isPortOpen())
				{
					MidiOutput_->closePort();
				}

				MidiOutput_->openPort(Index, OutputNames_[Index]);

				CurrentOutputIndex_ = Index;
				OutputOpen_ = true;
			}
			catch (const RtMidiError& Error)
			{
				CurrentOutputIndex_ = -1;
				OutputOpen_ = false;

				Error.printMessage();
				Log(Error.getMessage());
			}

			PublishSnapshot_Internal();
			break;
		}

		case EMidiCommandType::CloseOutput:
		{
			CloseOutput_Internal();
			break;
		}

		case EMidiCommandType::OpenInput:
		{
			OpenInput_Internal(Command.InputIndex);
			break;
		}

		case EMidiCommandType::CloseInput:
		{
			for (auto& Item : InputItems_)
			{
				if (Item && static_cast<int>(Item->Index) == Command.InputIndex)
				{
					CloseInput_Internal(*Item);
					break;
				}
			}

			break;
		}

		case EMidiCommandType::RefreshPorts:
		{
			RefreshPorts_Internal();
			break;
		}

		case EMidiCommandType::CheckPorts:
		{
			try
			{
				if (!MidiEnumerator_ || !MidiOutput_)
				{
					RefreshPorts_Internal();
					break;
				}

				const unsigned int CurrentInputCount = MidiEnumerator_->getPortCount();

				const unsigned int CurrentOutputCount = MidiOutput_->getPortCount();

				if (CurrentInputCount != InputPortCount_ || CurrentOutputCount != OutputPortCount_)
				{
					RefreshPorts_Internal();
				}
			}
			catch (const RtMidiError& Error)
			{
				Error.printMessage();
				Log(Error.getMessage());
			}

			break;
		}

		case EMidiCommandType::SetInputMuted:
		{
			for (auto& Item : InputItems_)
			{
				if (Item && static_cast<int>(Item->Index) == Command.InputIndex)
				{
					Item->Muted_atomic.store(Command.BoolValue, std::memory_order_relaxed);

					break;
				}
			}

			PublishSnapshot_Internal();
			break;
		}

		case EMidiCommandType::ReleaseAll:
		{
			ReleaseAll_Internal();
			break;
		}

		case EMidiCommandType::Shutdown:
		{
			ReleaseAll_Internal();
			ClearIncomingMidiQueue();

			{
				std::lock_guard<std::mutex> Lock(CommandMutex_);
				CommandQueue_.clear();
			}

			MidiThreadRunning_.store(false, std::memory_order_release);

			return;
		}
		}
	}
}

void GlueMidiWorker::RefreshPorts_Internal()
{
	if (!MidiEnumerator_ || !MidiOutput_)
	{
		Log("Cannot refresh MIDI ports: RtMidi is unavailable");
		return;
	}

	// Close and destroy all existing per-input RtMidi objects.
	for (auto& Item : InputItems_)
	{
		if (Item)
		{
			CloseInput_Internal(*Item);
		}
	}

	ClearIncomingMidiQueue();
	InputItems_.clear();

	CloseOutput_Internal();

	InputPortCount_ = 0;
	OutputPortCount_ = 0;
	OutputNames_.clear();

	try
	{
		InputPortCount_ = MidiEnumerator_->getPortCount();

		for (unsigned int Index = 0; Index < InputPortCount_; ++Index)
		{
			const std::string PortName = MidiEnumerator_->getPortName(Index);

			auto Item = std::make_unique<InputItem>(this, PortName, Index);

			Item->Id = NextInputId_++;

			InputItems_.push_back(std::move(Item));
		}
	}
	catch (const RtMidiError& Error)
	{
		Error.printMessage();
		Log(Error.getMessage());

		PublishSnapshot_Internal();
		return;
	}

	try
	{
		OutputPortCount_ = MidiOutput_->getPortCount();

		for (unsigned int Index = 0; Index < OutputPortCount_; ++Index)
		{
			const std::string PortName = MidiOutput_->getPortName(Index);

			const size_t LastSpace = PortName.find_last_of(' ');

			const std::string TrimmedName = LastSpace == std::string::npos
				? PortName
				: PortName.substr(0, LastSpace);

			OutputNames_.push_back(TrimmedName);
		}
	}
	catch (const RtMidiError& Error)
	{
		Error.printMessage();
		Log(Error.getMessage());

		PublishSnapshot_Internal();
		return;
	}

	Log("MIDI ports refreshed successfully");
	PublishSnapshot_Internal();
}

void GlueMidiWorker::OpenInput_Internal(const int InputIndex)
{
	InputItem* ItemPtr = nullptr;

	// Make sure we're using the right device
	for (auto& Candidate : InputItems_)
	{
		if (Candidate && static_cast<int>(Candidate->Index) == InputIndex)
		{
			ItemPtr = Candidate.get();
			break;
		}
	}

	if (ItemPtr == nullptr)
	{
		return;
	}

	InputItem& Item = *ItemPtr;

	if (Item.Active_atomic.load(std::memory_order_acquire))
	{
		return;
	}

	try
	{
		if (!Item.midiinput)
		{
			Item.midiinput = std::make_unique<RtMidiIn>();

			Item.midiinput->setBufferSize(2048, 4);
		}

		Item.AcceptCallbacks_atomic.store(false, std::memory_order_release);

		Item.midiinput->setCallback(MidiInputCallback, &Item);

		Item.midiinput->ignoreTypes(false, true, true);

		Item.midiinput->openPort(Item.Index, Item.NameIndexed);

		Item.Active_atomic.store(true, std::memory_order_release);

		Item.AcceptCallbacks_atomic.store(true, std::memory_order_release);

		Log("Input opened: " + Item.Name);
	}
	catch (const RtMidiError& Error)
	{
		Item.AcceptCallbacks_atomic.store(false, std::memory_order_release);

		Item.Active_atomic.store(false, std::memory_order_release);

		if (Item.midiinput)
		{
			try
			{
				Item.midiinput->cancelCallback();
			}
			catch (...)
			{
			}

			Item.midiinput.reset();
		}

		Error.printMessage();
		Log(Error.getMessage());
	}

	PublishSnapshot_Internal();
}

void GlueMidiWorker::CloseInput_Internal(
	InputItem& Input)
{
	Input.AcceptCallbacks_atomic.store(false, std::memory_order_release);

	if (Input.midiinput)
	{
		try
		{
			Input.midiinput->cancelCallback();

			if (Input.midiinput->isPortOpen())
			{
				Input.midiinput->closePort();
			}
		}
		catch (const RtMidiError& Error)
		{
			Error.printMessage();
			Log(Error.getMessage());
		}

		Input.midiinput.reset();
	}

	Input.Active_atomic.store(false, std::memory_order_release);

	Log("Input closed: " + Input.Name);

	PublishSnapshot_Internal();
}

void GlueMidiWorker::CloseOutput_Internal()
{
	try
	{
		if (MidiOutput_ && MidiOutput_->isPortOpen())
		{
			MidiOutput_->closePort();
		}
	}
	catch (const RtMidiError& Error)
	{
		Error.printMessage();
		Log(Error.getMessage());
	}

	CurrentOutputIndex_ = -1;
	OutputOpen_ = false;

	PublishSnapshot_Internal();
}

void GlueMidiWorker::ReleaseAll_Internal()
{
	for (auto& Item : InputItems_)
	{
		if (Item)
		{
			CloseInput_Internal(*Item);
		}
	}

	CloseOutput_Internal();

	ClearIncomingMidiQueue();

	PublishSnapshot_Internal();
}

void GlueMidiWorker::PublishSnapshot_Internal()
{
	MidiPortUiSnapshot NewSnapshot;

	NewSnapshot.Inputs.reserve(InputItems_.size());

	for (const auto& Item : InputItems_)
	{
		if (!Item)
		{
			continue;
		}

		MidiInputUiState UiState;
		UiState.Id = Item->Id;
		UiState.Index = static_cast<int>(Item->Index);
		UiState.Name = Item->Name;

		UiState.Active = Item->Active_atomic.load(std::memory_order_acquire);

		UiState.Muted = Item->Muted_atomic.load(std::memory_order_relaxed);

		UiState.LogMute = Item->LogMute;

		NewSnapshot.Inputs.push_back(std::move(UiState));
	}

	NewSnapshot.Outputs = OutputNames_;
	NewSnapshot.CurrentOutputIndex = CurrentOutputIndex_;
	NewSnapshot.OutputOpen = OutputOpen_;

	{
		std::lock_guard<std::mutex> Lock(SnapshotMutex_);

		Snapshot_ = std::move(NewSnapshot);
	}
}

void GlueMidiWorker::ClearIncomingMidiQueue()
{
	std::lock_guard<std::mutex> Lock(IncomingMidiMutex_);
	IncomingMidiQueue_.clear();
}

void GlueMidiWorker::SendMessageOnPort(const std::vector<unsigned char>& Message)
{
	if (!MidiOutput_ || !MidiOutput_->isPortOpen())
	{
		return;
	}

	try
	{
		MidiOutput_->sendMessage(&Message);
	}
	catch (const RtMidiError& Error)
	{
		Error.printMessage();
		Log(Error.getMessage());
	}
}

void GlueMidiWorker::Log(const std::string& Message) const
{
	if (LogCallback_)
	{
		LogCallback_(Message);
	}
}
