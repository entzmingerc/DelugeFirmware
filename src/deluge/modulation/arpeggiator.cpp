/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "modulation/arpeggiator.h"
#include "definitions_cxx.hpp"
#include "io/debug/log.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "storage/flash_storage.h"
#include "util/functions.h"
#include <stdint.h>

ArpeggiatorSettings::ArpeggiatorSettings() {
	numOctaves = 2;
	mode = ArpMode::OFF;
	noteMode = ArpNoteMode::UP;
	octaveMode = ArpOctaveMode::UP;
	preset = ArpPreset::OFF;
	flagForceArpRestart = false;

	// I'm so sorry, this is incredibly ugly, but in order to decide the default sync level, we have to look at the
	// current song, or even better the one being preloaded. Default sync level is used obviously for the default synth
	// sound if no SD card inserted, but also some synth presets, possibly just older ones, are saved without this so it
	// can be set to the default at the time of loading.
	Song* song = preLoadedSong;
	if (!song) {
		song = currentSong;
	}
	if (song) {
		syncLevel = (SyncLevel)(8 - (song->insideWorldTickMagnitude + song->insideWorldTickMagnitudeOffsetFromBPM));
	}
	else {
		syncLevel = (SyncLevel)(8 - FlashStorage::defaultMagnitude);
	}
	syncType = SYNC_TYPE_EVEN;
}

ArpeggiatorForDrum::ArpeggiatorForDrum() {
	ratchetingIsAvailable = false;
	arpNote.velocity = 0;
}

Arpeggiator::Arpeggiator() : notes(sizeof(ArpNote), 16, 0, 8, 8), notesAsPlayed(sizeof(ArpNote), 8, 8) {
	notes.emptyingShouldFreeMemory = false;
	notesAsPlayed.emptyingShouldFreeMemory = false;
}

// Surely this shouldn't be quite necessary?
void ArpeggiatorForDrum::reset() {
	arpNote.velocity = 0;
}

void ArpeggiatorBase::resetRatchet() {
	ratchetNotesIndex = 0;
	ratchetNotesMultiplier = 0;
	ratchetNotesNumber = 0;
	isRatcheting = false;
	D_PRINTLN("i %d m %d n %d b %d -> resetRatchet", ratchetNotesIndex, ratchetNotesMultiplier, ratchetNotesNumber,
	          isRatcheting);
}

void Arpeggiator::reset() {
	notes.empty();
	notesAsPlayed.empty();

	D_PRINTLN("Arpeggiator::reset");
	resetRatchet();
}

void ArpeggiatorForDrum::noteOn(ArpeggiatorSettings* settings, int32_t noteCode, int32_t velocity,
                                ArpReturnInstruction* instruction, int32_t fromMIDIChannel, int16_t const* mpeValues) {
	lastVelocity = velocity;

	bool wasActiveBefore = arpNote.velocity;

	arpNote.inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] = noteCode;
	arpNote.inputCharacteristics[util::to_underlying(MIDICharacteristic::CHANNEL)] = fromMIDIChannel;
	arpNote.velocity = velocity; // Means note is on.
	// MIDIInstrument might set this later, but it needs to be MIDI_CHANNEL_NONE until then so it doesn't get included
	// in the survey that will happen of existing output member channels.
	arpNote.outputMemberChannel = MIDI_CHANNEL_NONE;

	for (int32_t m = 0; m < kNumExpressionDimensions; m++) {
		arpNote.mpeValues[m] = mpeValues[m];
	}

	// If we're an actual arpeggiator...
	if ((settings != nullptr) && settings->mode != ArpMode::OFF) {

		// If this was the first note-on and we want to sound a note right now...
		if (!wasActiveBefore) {
			playedFirstArpeggiatedNoteYet = false;
			gateCurrentlyActive = false;

			if (!(playbackHandler.isEitherClockActive()) || !settings->syncLevel) {
				gatePos = 0;
				switchNoteOn(settings, instruction);
			}
		}

		// Don't do the note-on now, it'll happen automatically at next render
	}

	// Or otherwise, just switch the note on.
	else {
		instruction->noteCodeOnPostArp = noteCode;
		instruction->arpNoteOn = &arpNote;
	}
}

void ArpeggiatorForDrum::noteOff(ArpeggiatorSettings* settings, ArpReturnInstruction* instruction) {

	// If no arpeggiation...
	if ((settings == nullptr) || settings->mode == ArpMode::OFF) {
		instruction->noteCodeOffPostArp = kNoteForDrum;
		instruction->outputMIDIChannelOff = arpNote.outputMemberChannel;
	}

	// Or if yes arpeggiation...
	else {
		if (gateCurrentlyActive) {
			instruction->noteCodeOffPostArp = noteCodeCurrentlyOnPostArp;
			instruction->outputMIDIChannelOff = outputMIDIChannelForNoteCurrentlyOnPostArp;
		}
	}

	arpNote.velocity = 0; // Means note is off
}

// May return the instruction for a note-on, or no instruction. The noteCode instructed might be some octaves up from
// that provided here.
void Arpeggiator::noteOn(ArpeggiatorSettings* settings, int32_t noteCode, int32_t velocity,
                         ArpReturnInstruction* instruction, int32_t fromMIDIChannel, int16_t const* mpeValues) {

	lastVelocity = velocity;

	bool noteExists = false;

	ArpNote* arpNote;
	ArpNote* arpNoteAsPlayed;

	int32_t notesKey = notes.search(noteCode, GREATER_OR_EQUAL);
	if (notesKey < notes.getNumElements()) {
		arpNote = (ArpNote*)notes.getElementAddress(notesKey);
		if (arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] == noteCode) {
			noteExists = true;
		}
	}
	int32_t notesAsPlayedIndex = -1;
	for (int32_t i = 0; i < notesAsPlayed.getNumElements(); i++) {
		ArpNote* theArpNote = (ArpNote*)notesAsPlayed.getElementAddress(i);
		if (theArpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] == noteCode) {
			arpNoteAsPlayed = theArpNote;
			notesAsPlayedIndex = i;
			noteExists = true;
			break;
		}
	}

	if (noteExists) {
		// If note exists already, do nothing if we are an arpeggiator, and if not, go to noteinserted to update
		// midiChannel
		if ((settings != nullptr) && settings->mode != ArpMode::OFF) {
			return; // If we're an arpeggiator, return
		}
		else {
			goto noteInserted;
		}
	}
	// If note does not exist yet in the arrays, we must insert it in both
	else {
		// Insert in notes array
		int32_t error = notes.insertAtIndex(notesKey);
		if (error) {
			return;
		}
		// Save arpNote
		arpNote = (ArpNote*)notes.getElementAddress(notesKey);
		arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] = noteCode;
		arpNote->velocity = velocity;
		// MIDIInstrument might set this, but it needs to be MIDI_CHANNEL_NONE until then so it
		// doesn't get included in the survey that will happen of existing output member
		// channels.
		arpNote->outputMemberChannel = MIDI_CHANNEL_NONE;

		for (int32_t m = 0; m < kNumExpressionDimensions; m++) {
			arpNote->mpeValues[m] = mpeValues[m];
		}

		// Insert it in notesAsPlayed array
		notesAsPlayedIndex = notesAsPlayed.getNumElements();
		error = notesAsPlayed.insertAtIndex(notesAsPlayedIndex); // always insert at the end or the array
		if (error) {
			return;
		}
		// Save arpNote
		arpNoteAsPlayed = (ArpNote*)notesAsPlayed.getElementAddress(notesAsPlayedIndex);
		arpNoteAsPlayed->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] = noteCode;
		arpNoteAsPlayed->velocity = velocity;
		// MIDIInstrument might set this, but it needs to be MIDI_CHANNEL_NONE until then so it
		// doesn't get included in the survey that will happen of existing output member
		// channels.
		arpNoteAsPlayed->outputMemberChannel = MIDI_CHANNEL_NONE;

		for (int32_t m = 0; m < kNumExpressionDimensions; m++) {
			arpNoteAsPlayed->mpeValues[m] = mpeValues[m];
		}
	}

noteInserted:
	// This is here so that "stealing" a note being edited can then replace its MPE data during
	// editing. Kind of a hacky solution, but it works for now.
	arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::CHANNEL)] = fromMIDIChannel;
	arpNoteAsPlayed->inputCharacteristics[util::to_underlying(MIDICharacteristic::CHANNEL)] = fromMIDIChannel;

	// If we're an arpeggiator...
	if ((settings != nullptr) && settings->mode != ArpMode::OFF) {

		// If this was the first note-on and we want to sound a note right now...
		if (notes.getNumElements() == 1) {
			playedFirstArpeggiatedNoteYet = false;
			gateCurrentlyActive = false;

			if (!(playbackHandler.isEitherClockActive()) || !settings->syncLevel) {
				gatePos = 0;
				switchNoteOn(settings, instruction);
			}
		}

		// Or if the arpeggiator was already sounding
		else {
			if (whichNoteCurrentlyOnPostArp >= notesKey) {
				whichNoteCurrentlyOnPostArp++;
			}
		}
		// Don't do the note-on now, it'll happen automatically at next render
	}
	else {
		instruction->noteCodeOnPostArp = noteCode;
		instruction->arpNoteOn = arpNote;
	}
}

void Arpeggiator::noteOff(ArpeggiatorSettings* settings, int32_t noteCodePreArp, ArpReturnInstruction* instruction) {

	int32_t notesKey = notes.search(noteCodePreArp, GREATER_OR_EQUAL);
	if (notesKey < notes.getNumElements()) {

		ArpNote* arpNote = (ArpNote*)notes.getElementAddress(notesKey);
		if (arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] == noteCodePreArp) {

			// If no arpeggiation...
			if ((settings == nullptr) || settings->mode == ArpMode::OFF) {
				instruction->noteCodeOffPostArp = noteCodePreArp;
				instruction->outputMIDIChannelOff = arpNote->outputMemberChannel;
			}

			// Or if yes arpeggiation, we'll only stop right now if that was the last note to switch off. Otherwise,
			// it'll turn off soon with the arpeggiation.
			else {
				if (notes.getNumElements() == 1) {
					if (whichNoteCurrentlyOnPostArp == notesKey && gateCurrentlyActive) {
						instruction->noteCodeOffPostArp = noteCodeCurrentlyOnPostArp;
						instruction->outputMIDIChannelOff = outputMIDIChannelForNoteCurrentlyOnPostArp;
					}
				}
			}

			notes.deleteAtIndex(notesKey);
			// We must also search and delete from notesAsPlayed
			for (int32_t i = 0; i < notesAsPlayed.getNumElements(); i++) {
				ArpNote* arpNote = (ArpNote*)notesAsPlayed.getElementAddress(i);
				if (arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] == noteCodePreArp) {
					notesAsPlayed.deleteAtIndex(i);
					break;
				}
			}

			if (whichNoteCurrentlyOnPostArp >= notesKey) {
				whichNoteCurrentlyOnPostArp--; // Beware - this could send it negative
				if (whichNoteCurrentlyOnPostArp < 0) {
					whichNoteCurrentlyOnPostArp = 0;
				}
			}

			if (isRatcheting && (ratchetNotesIndex >= ratchetNotesNumber || !playbackHandler.isEitherClockActive())) {
				// If it was ratcheting but it reached the last note in the ratchet or play was stopped
				// then we can reset the ratchet temp values.
				D_PRINTLN("noteOff 1");
				resetRatchet();
			}
		}
	}

	if (notes.getNumElements() == 0) {
		D_PRINTLN("noteOff 2");
		resetRatchet();
	}
}

void ArpeggiatorBase::switchAnyNoteOff(ArpReturnInstruction* instruction) {
	if (gateCurrentlyActive) {
		// triggerable->noteOffPostArpeggiator(modelStack, noteCodeCurrentlyOnPostArp);
		instruction->noteCodeOffPostArp = noteCodeCurrentlyOnPostArp;
		instruction->outputMIDIChannelOff = outputMIDIChannelForNoteCurrentlyOnPostArp;
		gateCurrentlyActive = false;

		if (isRatcheting && (ratchetNotesIndex >= ratchetNotesNumber || !playbackHandler.isEitherClockActive())) {
			// If it was ratcheting but it reached the last note in the ratchet or play was stopped
			// then we can reset the ratchet temp values.
			D_PRINTLN("switchAnyNoteOff");
			resetRatchet();
		}
	}
}

void ArpeggiatorBase::maybeSetupNewRatchet(ArpeggiatorSettings* settings) {
	int32_t randomChance = random(65535);
	isRatcheting = ratchetProbability > randomChance && ratchetAmount > 0;
	if (isRatcheting) {
		ratchetNotesMultiplier = random(65535) % (ratchetAmount + 1);
		ratchetNotesNumber = 1 << ratchetNotesMultiplier;
		if (settings->syncLevel == SyncLevel::SYNC_LEVEL_256TH) {
			// If the sync level is 256th, we can't have a ratchet of more than 2 notes, so we set it to the minimum
			ratchetNotesMultiplier = 1;
			ratchetNotesNumber = 2;
		}
		else if (settings->syncLevel == SyncLevel::SYNC_LEVEL_128TH) {
			// If the sync level is 128th, the maximum ratchet can be of 4 notes (8 not allowed)
			ratchetNotesMultiplier = std::max((uint8_t)2, ratchetNotesMultiplier);
			ratchetNotesNumber = std::max((uint8_t)4, ratchetNotesNumber);
		}
	}
	else {
		ratchetNotesMultiplier = 0;
		ratchetNotesNumber = 0;
	}
	ratchetNotesIndex = 0;
	D_PRINTLN("i %d m %d n %d b %d -> maybeSetupNewRatchet", ratchetNotesIndex, ratchetNotesMultiplier,
	          ratchetNotesNumber, isRatcheting);
}

void ArpeggiatorBase::carryOnSequenceForSingleNoteArpeggio(ArpeggiatorSettings* settings) {
	if (settings->numOctaves == 1) {
		currentOctave = 0;
		currentOctaveDirection = 1;
	}
	else if (settings->octaveMode == ArpOctaveMode::RANDOM) {
		currentOctave = getRandom255() % settings->numOctaves;
		currentOctaveDirection = 1;
	}
	else if (settings->octaveMode == ArpOctaveMode::UP_DOWN || settings->octaveMode == ArpOctaveMode::ALTERNATE) {
		currentOctave += currentOctaveDirection;
		if (currentOctave > settings->numOctaves - 1) {
			// Now go down
			currentOctaveDirection = -1;
			if (settings->octaveMode == ArpOctaveMode::ALTERNATE) {
				currentOctave = settings->numOctaves - 2;
			}
			else {
				currentOctave = settings->numOctaves - 1;
			}
		}
		else if (currentOctave < 0) {
			// Now go up
			currentOctaveDirection = 1;
			if (settings->octaveMode == ArpOctaveMode::ALTERNATE) {
				currentOctave = 1;
			}
			else {
				currentOctave = 0;
			}
		}
	}
	else {
		// Have to reset this, in case the user changed the setting.
		currentOctaveDirection = (settings->octaveMode == ArpOctaveMode::DOWN) ? -1 : 1;
		currentOctave += currentOctaveDirection;
		if (currentOctave >= settings->numOctaves) {
			currentOctave = 0;
		}
		else if (currentOctave < 0) {
			currentOctave = settings->numOctaves - 1;
		}
	}
}

void ArpeggiatorForDrum::switchNoteOn(ArpeggiatorSettings* settings, ArpReturnInstruction* instruction) {

	// Note: for drum arpeggiator, the note mode is irrelevant, so we don't need to check it here.
	//  We only need to account for octaveMode as it is always a 1-note arpeggio.
	//  Besides, behavior of OctaveMode::UP_DOWN is equal to OctaveMode::ALTERNATE

	gateCurrentlyActive = true;

	// If RANDOM, we do the same thing whether playedFirstArpeggiatedNoteYet or not
	if (settings->octaveMode == ArpOctaveMode::RANDOM) {
		currentOctave = getRandom255() % settings->numOctaves;
		currentOctaveDirection = 1;
	}
	// Or not RANDOM
	else {
		if (maxSequenceLength > 0 && notesPlayedFromSequence >= maxSequenceLength) {
			playedFirstArpeggiatedNoteYet = false;
		}

		// If which-note not actually set up yet...
		if (!playedFirstArpeggiatedNoteYet) {
			notesPlayedFromSequence = 0;
			// Set the initial octave
			if (settings->octaveMode == ArpOctaveMode::DOWN) {
				currentOctave = settings->numOctaves - 1;
				currentOctaveDirection = -1;
			}
			else {
				currentOctave = 0;
				currentOctaveDirection = 1;
			}
		}

		// Otherwise, just carry on the sequence of arpeggiated notes
		else {
			carryOnSequenceForSingleNoteArpeggio(settings);
		}
	}

	playedFirstArpeggiatedNoteYet = true;
	notesPlayedFromSequence++;

	noteCodeCurrentlyOnPostArp = kNoteForDrum + (int32_t)currentOctave * 12;

	instruction->noteCodeOnPostArp = noteCodeCurrentlyOnPostArp;
	instruction->arpNoteOn = &arpNote;
}

void Arpeggiator::switchNoteOn(ArpeggiatorSettings* settings, ArpReturnInstruction* instruction) {

	gateCurrentlyActive = true;

	if (ratchetNotesIndex > 0) {
		goto finishSwitchNoteOn;
	}

	// If FULL-RANDOM (RANDOM for both Note and Octave), we do the same thing whether playedFirstArpeggiatedNoteYet or
	// not
	if (settings->noteMode == ArpNoteMode::RANDOM && settings->octaveMode == ArpOctaveMode::RANDOM) {
		whichNoteCurrentlyOnPostArp = getRandom255() % (uint8_t)notes.getNumElements();
		currentOctave = getRandom255() % settings->numOctaves;

		// Must set all these variables here, even though RANDOM
		// doesn't use them, in case user changes arp mode.
		notesPlayedFromSequence = 0;
		randomNotesPlayedFromOctave = 0;
		currentOctaveDirection = 1;
		currentDirection = 1;
	}

	// Or not FULL-RANDOM
	else {
		if (maxSequenceLength > 0 && notesPlayedFromSequence >= maxSequenceLength) {
			playedFirstArpeggiatedNoteYet = false;
		}

		// If which-note not actually set up yet...
		if (!playedFirstArpeggiatedNoteYet) {
			// Set initial values for note and octave

			// NOTE
			notesPlayedFromSequence = 0;
			randomNotesPlayedFromOctave = 0;
			if (settings->noteMode == ArpNoteMode::RANDOM) {
				// Note: Random
				whichNoteCurrentlyOnPostArp = getRandom255() % (uint8_t)notes.getNumElements();
				currentDirection = 1;
			}
			else if (settings->noteMode == ArpNoteMode::DOWN) {
				// Note: Down
				whichNoteCurrentlyOnPostArp = notes.getNumElements() - 1;
				currentDirection = -1;
			}
			else {
				// Note: Up or Up&Down
				whichNoteCurrentlyOnPostArp = 0;
				currentDirection = 1;
			}

			// OCTAVE
			if (settings->octaveMode == ArpOctaveMode::RANDOM) {
				// Octave: Random
				currentOctave = getRandom255() % settings->numOctaves;
				currentOctaveDirection = 1;
			}
			else if (settings->octaveMode == ArpOctaveMode::DOWN
			         || (settings->octaveMode == ArpOctaveMode::ALTERNATE && settings->noteMode == ArpNoteMode::DOWN)) {
				// Octave: Down or Alternate (with note down)
				currentOctave = settings->numOctaves - 1;
				currentOctaveDirection = -1;
			}
			else {
				// Octave: Up or Up&Down or Alternate (with note up)
				currentOctave = 0;
				currentOctaveDirection = 1;
			}
		}

		// For 1-note arpeggios it is simpler and can use the same logic as for drums
		else if (notes.getNumElements() == 1) {
			carryOnSequenceForSingleNoteArpeggio(settings);
		}

		// Otherwise, just carry on the sequence of arpeggiated notes
		else {

			// Arpeggios of more than 1 note

			// NOTE
			bool changeOctave = false;
			bool changingOctaveDirection = false;
			if (settings->noteMode == ArpNoteMode::RANDOM) {
				// Note: Random
				whichNoteCurrentlyOnPostArp = getRandom255() % (uint8_t)notes.getNumElements();
				if (randomNotesPlayedFromOctave >= notes.getNumElements()) {
					changeOctave = true;
				}
			}
			else {
				whichNoteCurrentlyOnPostArp += currentDirection;

				// If reached top of notes (so current direction must be up)
				if (whichNoteCurrentlyOnPostArp >= notes.getNumElements()) {
					changingOctaveDirection =
					    (int32_t)currentOctave >= settings->numOctaves - 1
					    && (settings->noteMode == ArpNoteMode::UP || settings->noteMode == ArpNoteMode::AS_PLAYED
					        || settings->noteMode == ArpNoteMode::DOWN)
					    && settings->octaveMode == ArpOctaveMode::ALTERNATE;
					if (changingOctaveDirection) {
						// Now go down (without repeating)
						currentDirection = -1;
						whichNoteCurrentlyOnPostArp -= 2;
					}
					else if (settings->noteMode == ArpNoteMode::UP_DOWN) {
						// Now go down (repeating note)
						currentDirection = -1;
						whichNoteCurrentlyOnPostArp -= 1;
					}
					else { // Up or AsPlayed
						// Start on next octave first note
						whichNoteCurrentlyOnPostArp = 0;
						changeOctave = true;
					}
				}

				// Or, if reached bottom of notes (so current direction must be down)
				else if (whichNoteCurrentlyOnPostArp < 0) {
					changingOctaveDirection =
					    (int32_t)currentOctave <= 0
					    && (settings->noteMode == ArpNoteMode::UP || settings->noteMode == ArpNoteMode::AS_PLAYED
					        || settings->noteMode == ArpNoteMode::DOWN)
					    && settings->octaveMode == ArpOctaveMode::ALTERNATE;
					if (changingOctaveDirection) {
						// Now go up
						currentDirection = 1;
						whichNoteCurrentlyOnPostArp += 2;
					}
					else if (settings->noteMode == ArpNoteMode::UP_DOWN) { // UpDown
						// Start on next octave first note
						whichNoteCurrentlyOnPostArp = 0;
						currentDirection = 1;
						changeOctave = true;
					}
					else { // Down
						whichNoteCurrentlyOnPostArp = notes.getNumElements() - 1;
						changeOctave = true;
					}
				}
			}

			// OCTAVE
			if (changingOctaveDirection) {
				currentOctaveDirection = currentOctaveDirection == -1 ? 1 : -1;
			}
			if (changeOctave) {
				randomNotesPlayedFromOctave = 0; // reset this in any case
				carryOnSequenceForSingleNoteArpeggio(settings);
			}
		}
	}

	// Only increase steps played from the sequence for normal notes (not for ratchet notes)
	notesPlayedFromSequence++;

finishSwitchNoteOn:
	playedFirstArpeggiatedNoteYet = true;

#if ALPHA_OR_BETA_VERSION
	if (whichNoteCurrentlyOnPostArp < 0 || whichNoteCurrentlyOnPostArp >= notes.getNumElements()) {
		FREEZE_WITH_ERROR("E404");
	}
#endif
	randomNotesPlayedFromOctave++;

	ArpNote* arpNote;
	if (settings->noteMode == ArpNoteMode::AS_PLAYED) {
		arpNote = (ArpNote*)notesAsPlayed.getElementAddress(whichNoteCurrentlyOnPostArp);
	}
	else {
		arpNote = (ArpNote*)notes.getElementAddress(whichNoteCurrentlyOnPostArp);
	}

	noteCodeCurrentlyOnPostArp =
	    arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] + (int32_t)currentOctave * 12;

	instruction->noteCodeOnPostArp = noteCodeCurrentlyOnPostArp;
	instruction->arpNoteOn = arpNote;

	// Increment ratchet note index if we are ratcheting
	if (isRatcheting) {
		ratchetNotesIndex++;
		D_PRINTLN("i %d m %d n %d b %d -> switchNoteOn RATCHETING", ratchetNotesIndex, ratchetNotesMultiplier,
		          ratchetNotesNumber, isRatcheting);
	}
	else {
		D_PRINTLN("i %d m %d n %d b %d -> switchNoteOn NORMAL", ratchetNotesIndex, ratchetNotesMultiplier,
		          ratchetNotesNumber, isRatcheting);
	}
}

bool Arpeggiator::hasAnyInputNotesActive() {
	return notes.getNumElements();
}

bool ArpeggiatorForDrum::hasAnyInputNotesActive() {
	return arpNote.velocity;
}

// Check arpeggiator is on before you call this.
// May switch notes on and/or off.
void ArpeggiatorBase::render(ArpeggiatorSettings* settings, int32_t numSamples, uint32_t gateThreshold,
                             uint32_t phaseIncrement, uint32_t sequenceLength, uint32_t ratchAmount, uint32_t ratchProb,
                             ArpReturnInstruction* instruction) {
	if (settings->mode == ArpMode::OFF || !hasAnyInputNotesActive()) {
		return;
	}

	uint32_t gateThresholdSmall = gateThreshold >> 8;

	// Update Sequence Length
	maxSequenceLength = sequenceLength;

	// Update ratchetProbability with the most up to date value from automation
	ratchetProbability = ratchProb >> 16; // just 16 bits is enough resolution for probability

	// Convert ratchAmount to either 0, 1, 2 or 3 (equivalent to a number of ratchets: OFF, 2, 4, 8)
	uint16_t amount = ratchAmount >> 16;
	if (amount > 45874) {
		ratchetAmount = 3;
	}
	else if (amount > 26214) {
		ratchetAmount = 2;
	}
	else if (amount > 6553) {
		ratchetAmount = 1;
	}
	else {
		ratchetAmount = 0;
	}

	if (isRatcheting) {
		// shorten gate in case we are ratcheting (with the calculated number of ratchet notes)
		gateThresholdSmall = gateThresholdSmall >> ratchetNotesMultiplier;
	}

	bool syncedNow = (settings->syncLevel && (playbackHandler.isEitherClockActive()));

	// If gatePos is far enough along that we at least want to switch off any note...
	if (gatePos >= gateThresholdSmall) {
		switchAnyNoteOff(instruction);

		// And maybe (if not syncing) the gatePos is also far enough along that we also want to switch a note on?
		if (!syncedNow && gatePos >= 16777216) {
			switchNoteOn(settings, instruction);
		}
	}

	if (!syncedNow) {
		gatePos &= 16777215;
	}

	gatePos += (phaseIncrement >> 8) * numSamples;
}

void ArpeggiatorBase::setRatchetingAvailable(bool available) {
	ratchetingIsAvailable = available;
}

// Returns num ticks til we next want to come back here.
// May switch notes on and/or off.
int32_t ArpeggiatorBase::doTickForward(ArpeggiatorSettings* settings, ArpReturnInstruction* instruction,
                                       uint32_t clipCurrentPos, bool currentlyPlayingReversed) {
	// Make sure we actually intended to sync
	if (settings->mode == ArpMode::OFF || (settings->syncLevel == 0u)) {
		return 2147483647;
	}

	if (settings->flagForceArpRestart) {
		// If flagged to restart sequence, do it now and reset the flag
		playedFirstArpeggiatedNoteYet = false;
		settings->flagForceArpRestart = false;
	}

	uint32_t ticksPerPeriod = 3 << (9 - settings->syncLevel);
	if (settings->syncType == SYNC_TYPE_EVEN) {} // Do nothing
	else if (settings->syncType == SYNC_TYPE_TRIPLET) {
		ticksPerPeriod = ticksPerPeriod * 2 / 3;
	}
	else if (settings->syncType == SYNC_TYPE_DOTTED) {
		ticksPerPeriod = ticksPerPeriod * 3 / 2;
	}

	if (ratchetingIsAvailable) {
		if (!isRatcheting) {
			// If we are not ratcheting yet, check if we should and set it up (based on ratchet chance)
			maybeSetupNewRatchet(settings);
		}

		// If in previous step we set up ratcheting, we need to recalculate ticksPerPeriod
		if (isRatcheting) {
			ticksPerPeriod = ticksPerPeriod >> ratchetNotesMultiplier;
		}
	}

	int32_t howFarIntoPeriod = clipCurrentPos % ticksPerPeriod;

	if (!howFarIntoPeriod) {
		if (hasAnyInputNotesActive()) {
			switchAnyNoteOff(instruction);
			switchNoteOn(settings, instruction);

			instruction->sampleSyncLengthOn = ticksPerPeriod; // Overwrite this
			gatePos = 0;
		}
		howFarIntoPeriod = ticksPerPeriod;
	}
	else {
		if (!currentlyPlayingReversed) {
			howFarIntoPeriod = ticksPerPeriod - howFarIntoPeriod;
		}
	}
	return howFarIntoPeriod; // Normally we will have modified this variable above, and it no longer represents what its
	                         // name says.
}

uint32_t ArpeggiatorSettings::getPhaseIncrement(int32_t arpRate) {
	uint32_t phaseIncrement;
	if (syncLevel == 0) {
		phaseIncrement = arpRate >> 5;
	}
	else {
		int32_t rightShiftAmount = 9 - syncLevel; // Will be max 0
		phaseIncrement =
		    playbackHandler
		        .getTimePerInternalTickInverse(); // multiply_32x32_rshift32(playbackHandler.getTimePerInternalTickInverse(),
		                                          // arpRate);
		phaseIncrement >>= rightShiftAmount;
	}
	return phaseIncrement;
}
