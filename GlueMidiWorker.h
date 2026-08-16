#pragma once

#include "GlueMidiStructs.h"
#include "RtMidi.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class GlueMidiWorker
{
public:
	using LogCallback = std::function<void(const std::string&)>;

	explicit GlueMidiWorker(LogCallback InLogCallback);
	~GlueMidiWorker();

	void Start();
	void Stop();

	// generic enqueuer
	void QueueCommand(MidiCommand Command);	

	void QueueOpenOutput(int OutputIndex);
	void QueueCloseOutput();

	void QueueOpenInput(int InputIndex);
	void QueueCloseInput(int InputIndex);

	void QueueCheckPorts();
	void QueueRefreshPorts();

	void QueueSetInputMuted(
		int InputIndex,
		bool Muted);

	void QueueReleaseAll();

	MidiPortUiSnapshot GetSnapshot() const;

	std::vector<QueuedMidiMessage>
		DrainMonitorMessages();

	// Called only by RtMidi's input callbacks.
	void EnqueueMidiMessage(uint64_t InputId, double DeltaTime, bool ShouldRoute, const std::vector<unsigned char>& Message);

private:
	void ThreadMain();

	bool HasPendingCommands();
	void ProcessPendingCommands();

	void RefreshPorts_Internal();
	void OpenInput_Internal(int InputIndex);
	void CloseInput_Internal(InputItem& Input);
	void CloseOutput_Internal();
	void ReleaseAll_Internal();

	void PublishSnapshot_Internal();
	void ClearIncomingMidiQueue();

	void SendMessageOnPort(
		const std::vector<unsigned char>& Message);

	void Log(const std::string& Message) const;

private:
	
	LogCallback LogCallback_;

	uint64_t NextInputId_ = 1;

	std::thread MidiThread_;
	std::atomic<bool> MidiThreadRunning_{ false };

	// Commands and callback MIDI packets use the same wake-up condition.
	std::condition_variable WorkerCondition_;

	std::mutex CommandMutex_;
	std::deque<MidiCommand> CommandQueue_;

	std::mutex IncomingMidiMutex_;
	std::deque<QueuedMidiMessage> IncomingMidiQueue_;

	std::mutex MonitorMidiMutex_;
	std::deque<QueuedMidiMessage> MonitorMidiQueue_;

	mutable std::mutex SnapshotMutex_;
	MidiPortUiSnapshot Snapshot_;

	// Constructed, accessed and destroyed only by MidiThread_.
	std::unique_ptr<RtMidiIn> MidiEnumerator_;
	std::unique_ptr<RtMidiOut> MidiOutput_;

	std::vector<std::unique_ptr<InputItem>> InputItems_;
	std::vector<std::string> OutputNames_;

	unsigned int InputPortCount_ = 0;
	unsigned int OutputPortCount_ = 0;

	bool OutputOpen_ = false;
	int CurrentOutputIndex_ = -1;
};