# GlueMidi

## Route MIDI messages from multiple hardware or virtual ports to a single output device or port.

* Hot-plug friendly
* Can live unobtrusively in the system tray
* Icon animates when traffic is being processed
* Handy MIDI monitor 


### About
This app replicates the single thing that I still use MIDI-OX for daily, but with the aim of being a bit less crashy and a lot more amenable to the hotplugging of devices. MIDI-OX remains a Swiss army knife for all things MIDI, and it's very cool that it still works so many years after it was released, and 15 years after it was last updated.

My use-case is that I like my main DAWs of choice (Ableton Live and Reaper) as well as my own custom Unreal Engine audio middleware to have loopMIDI set as their principal MIDI input. loopMIDI is a great multi-client virtual MIDI device. Most devices/drivers are not multi-client, so feeding other devices through a single loopMIDI port lets me get e.g. my master MIDI keyboard, my Lightpad MPE thingy and my Fader3 USB controller all sending data to the same endpoint and accessible wherever I need it.

I wrote it for myself, and it works for me in my everyday workflow, but feel free to post bugs if you find them - bearing in mind that I'll probably not have time to deal with anything major. 

It's Windows-only because a) other operating systems are well served for this kind of functionality and b) it uses a hell of a lot of Win32 API weirdness that I'm not interested in making portable.

As usual, go nuts with it but don't blame me if it doesn't work or breaks something else somehow. 