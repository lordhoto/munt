/* Copyright (C) 2003-2009 Dean Beeler, Jerome Fisher
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <math.h>

#include "mt32emu.h"

namespace MT32Emu {

static const Bit8u PartialStruct[13] = {
	0, 0, 2, 2, 1, 3,
	3, 0, 3, 0, 2, 1, 3 };

static const Bit8u PartialMixStruct[13] = {
	0, 1, 0, 1, 1, 0,
	1, 3, 3, 2, 2, 2, 2 };

static const float floatKeyfollow[17] = {
	-1.0f, -1.0f/2.0f, -1.0f/4.0f, 0.0f,
	1.0f/8.0f, 1.0f/4.0f, 3.0f/8.0f, 1.0f/2.0f, 5.0f/8.0f, 3.0f/4.0f, 7.0f/8.0f, 1.0f,
	5.0f/4.0f, 3.0f/2.0f, 2.0f,
	1.0009765625f, 1.0048828125f
};


RhythmPart::RhythmPart(Synth *useSynth, unsigned int usePartNum): Part(useSynth, usePartNum) {
	strcpy(name, "Rhythm");
	rhythmTemp = &synth->mt32ram.rhythmTemp[0];
	refresh();
}

Part::Part(Synth *useSynth, unsigned int usePartNum) {
	this->synth = useSynth;
	this->partNum = usePartNum;
	patchCache[0].dirty = true;
	holdpedal = false;
	patchTemp = &synth->mt32ram.patchTemp[partNum];
	if (usePartNum == 8) {
		// Nasty hack for rhythm
		timbreTemp = NULL;
	} else {
		sprintf(name, "Part %d", partNum + 1);
		timbreTemp = &synth->mt32ram.timbreTemp[partNum];
	}
	currentInstr[0] = 0;
	currentInstr[10] = 0;
	midiExpression = 127;
	expression = 100;
	volumeMult = 0;
	volumesetting.leftvol = 32767;
	volumesetting.rightvol = 32767;
	bend = 0.0f;
	memset(polyTable,0,sizeof(polyTable));
	memset(patchCache, 0, sizeof(patchCache));
}

void Part::setHoldPedal(bool pedalval) {
	if (holdpedal && !pedalval) {
		holdpedal = false;
		stopPedalHold();
	} else {
		holdpedal = pedalval;
	}
}

Bit32s Part::getPitchBend() const {
	return pitchBend;
}

void Part::setBend(unsigned int midiBend) {
	// CONFIRMED:
	pitchBend = ((midiBend - 8192) * pitchBenderRange) >> 14;

	// DEPRECATED: Everything below will soon be obsolete and removed
	if (midiBend <= 0x2000) {
		bend = ((signed int)midiBend - 0x2000) / (float)0x2000;
	} else {
		bend = ((signed int)midiBend - 0x2000) / (float)0x1FFF;
	}
	for (int i = 0; i < MT32EMU_MAX_POLY; i++) {
		for (int j = 0; j < 4; j++) {
			if (polyTable[i].partials[j] != NULL) {
				polyTable[i].partials[j]->setBend(bend);
			}
		}
	}
}

Bit8u Part::getModulation() const {
	return modulation;
}

void Part::setModulation(unsigned int midiModulation) {
	modulation = (Bit8u)midiModulation;

	// DEPRECATED: Everything below will soon be obsolete and removed
	for (int t = 0; t < 4; t++) {
		if (patchCache[t].playPartial) {
			int newrate = (patchCache[t].modsense * midiModulation) >> 7;
			patchCache[t].lfodepth = newrate;
		}
	}
}

void RhythmPart::refresh() {
	updateVolume();
	// (Re-)cache all the mapped timbres ahead of time
	for (unsigned int drumNum = 0; drumNum < synth->controlROMMap->rhythmSettingsCount; drumNum++) {
		int drumTimbreNum = rhythmTemp[drumNum].timbre;
		if (drumTimbreNum >= 127) // 94 on MT-32
			continue;
		Bit16s pan = rhythmTemp[drumNum].panpot; // They use R-L 0-14...
		// FIXME:KG: Panning cache should be backed up to partials using it, too
		if (pan < 7) {
			drumPan[drumNum].leftvol = pan * 4681;
			drumPan[drumNum].rightvol = 32767;
		} else {
			drumPan[drumNum].rightvol = (14 - pan) * 4681;
			drumPan[drumNum].leftvol = 32767;
		}
		PatchCache *cache = drumCache[drumNum];
		backupCacheToPartials(cache);
		for (int t = 0; t < 4; t++) {
			// Common parameters, stored redundantly
			cache[t].dirty = true;
			cache[t].pitchShift = 0.0f;
			cache[t].benderRange = 0.0f;
			cache[t].pansetptr = &drumPan[drumNum];
			cache[t].reverb = rhythmTemp[drumNum].reverbSwitch > 0;
		}
	}
}

void Part::refresh() {
	updateVolume();
	backupCacheToPartials(patchCache);
	for (int t = 0; t < 4; t++) {
		// Common parameters, stored redundantly
		patchCache[t].dirty = true;
		patchCache[t].pitchShift = (patchTemp->patch.keyShift - 24) + (patchTemp->patch.fineTune - 50) / 100.0f;
		patchCache[t].benderRange = patchTemp->patch.benderRange;
		patchCache[t].pansetptr = &volumesetting;
		patchCache[t].reverb = patchTemp->patch.reverbSwitch > 0;
	}
	memcpy(currentInstr, timbreTemp->common.name, 10);
}

const char *Part::getCurrentInstr() const {
	return &currentInstr[0];
}

void RhythmPart::refreshTimbre(unsigned int absTimbreNum) {
	for (int m = 0; m < 85; m++) {
		if (rhythmTemp[m].timbre == absTimbreNum - 128)
			drumCache[m][0].dirty = true;
	}
}

void Part::refreshTimbre(unsigned int absTimbreNum) {
	if (getAbsTimbreNum() == absTimbreNum) {
		memcpy(currentInstr, timbreTemp->common.name, 10);
		patchCache[0].dirty = true;
	}
}

int Part::fixBiaslevel(int srcpnt, int *dir) {
	int noteat = srcpnt & 0x3F;
	int outnote;
	if (srcpnt < 64)
		*dir = 0;
	else
		*dir = 1;
	outnote = 33 + noteat;
	//synth->printDebug("Bias note %d, dir %d", outnote, *dir);

	return outnote;
}

int Part::fixKeyfollow(int srckey) {
	if (srckey>=0 && srckey<=16) {
		int keyfix[17] = { -256*16, -128*16, -64*16, 0, 32*16, 64*16, 96*16, 128*16, (128+32)*16, 192*16, (192+32)*16, 256*16, (256+64)*16, (256+128)*16, (512)*16, 4100, 4116};
		return keyfix[srckey];
	} else {
		//LOG(LOG_ERROR|LOG_MISC,"Missed key: %d", srckey);
		return 256;
	}
}

void Part::abortPoly(Poly *poly) {
	if (!poly->isPlaying) {
		return;
	}
	for (int i = 0; i < 4; i++) {
		Partial *partial = poly->partials[i];
		if (partial != NULL) {
			partial->deactivate();
		}
	}
	poly->isPlaying = false;
}

void Part::setPatch(const PatchParam *patch) {
	patchTemp->patch = *patch;
}

void RhythmPart::setTimbre(TimbreParam * /*timbre*/) {
	synth->printDebug("%s: Attempted to call setTimbre() - doesn't make sense for rhythm", name);
}

void Part::setTimbre(TimbreParam *timbre) {
	*timbreTemp = *timbre;
}

unsigned int RhythmPart::getAbsTimbreNum() const {
	synth->printDebug("%s: Attempted to call getAbsTimbreNum() - doesn't make sense for rhythm", name);
	return 0;
}

unsigned int Part::getAbsTimbreNum() const {
	return (patchTemp->patch.timbreGroup * 64) + patchTemp->patch.timbreNum;
}

void RhythmPart::setProgram(unsigned int patchNum) {
	synth->printDebug("%s: Attempt to set program (%d) on rhythm is invalid", name, patchNum);
}

void Part::updatePitchBenderRange() {
	pitchBenderRange = patchTemp->patch.benderRange * 683;
}

void Part::setProgram(unsigned int patchNum) {
	setPatch(&synth->mt32ram.patches[patchNum]);
	updatePitchBenderRange();
	setTimbre(&synth->mt32ram.timbres[getAbsTimbreNum()].timbre);

	refresh();

	allSoundOff(); //FIXME:KG: Is this correct?
}

void Part::backupCacheToPartials(PatchCache cache[4]) {
	// check if any partials are still playing with the old patch cache
	// if so then duplicate the cached data from the part to the partial so that
	// we can change the part's cache without affecting the partial.
	// We delay this until now to avoid a copy operation with every note played
	for (int m = 0; m < MT32EMU_MAX_POLY; m++) {
		for (int i = 0; i < 4; i++) {
			Partial *partial = polyTable[m].partials[i];
			if (partial != NULL && partial->patchCache == &cache[i]) {
				partial->cachebackup = cache[i];
				partial->patchCache = &partial->cachebackup;
			}
		}
	}
}

void Part::cacheTimbre(PatchCache cache[4], const TimbreParam *timbre) {
	backupCacheToPartials(cache);
	int partialCount = 0;
	for (int t = 0; t < 4; t++) {
		if (((timbre->common.partialMute >> t) & 0x1) == 1) {
			cache[t].playPartial = true;
			partialCount++;
		} else {
			cache[t].playPartial = false;
			continue;
		}

		// Calculate and cache common parameters
		cache[t].srcPartial = timbre->partial[t];

		cache[t].pcm = timbre->partial[t].wg.pcmWave;
		cache[t].useBender = (timbre->partial[t].wg.pitchBenderEnabled == 1);

		switch (t) {
		case 0:
			cache[t].PCMPartial = (PartialStruct[(int)timbre->common.partialStructure12] & 0x2) ? true : false;
			cache[t].structureMix = PartialMixStruct[(int)timbre->common.partialStructure12];
			cache[t].structurePosition = 0;
			cache[t].structurePair = 1;
			break;
		case 1:
			cache[t].PCMPartial = (PartialStruct[(int)timbre->common.partialStructure12] & 0x1) ? true : false;
			cache[t].structureMix = PartialMixStruct[(int)timbre->common.partialStructure12];
			cache[t].structurePosition = 1;
			cache[t].structurePair = 0;
			break;
		case 2:
			cache[t].PCMPartial = (PartialStruct[(int)timbre->common.partialStructure34] & 0x2) ? true : false;
			cache[t].structureMix = PartialMixStruct[(int)timbre->common.partialStructure34];
			cache[t].structurePosition = 0;
			cache[t].structurePair = 3;
			break;
		case 3:
			cache[t].PCMPartial = (PartialStruct[(int)timbre->common.partialStructure34] & 0x1) ? true : false;
			cache[t].structureMix = PartialMixStruct[(int)timbre->common.partialStructure34];
			cache[t].structurePosition = 1;
			cache[t].structurePair = 2;
			break;
		default:
			break;
		}

		cache[t].waveform = timbre->partial[t].wg.waveform;
		cache[t].pulsewidth = timbre->partial[t].wg.pulseWidth;
		cache[t].pwsens = timbre->partial[t].wg.pulseWidthVeloSensitivity;
		if (timbre->partial[t].wg.pitchKeyfollow > 16) {
			synth->printDebug("Bad keyfollow value in timbre!");
			cache[t].pitchKeyfollow = 1.0f;
		} else {
			cache[t].pitchKeyfollow = floatKeyfollow[timbre->partial[t].wg.pitchKeyfollow];
		}

		cache[t].pitch = timbre->partial[t].wg.pitchCoarse + (timbre->partial[t].wg.pitchFine - 50) / 100.0f + 24.0f;
		cache[t].pitchEnv = timbre->partial[t].pitchEnv;
		cache[t].pitchEnv.veloSensitivity = (char)((float)cache[t].pitchEnv.veloSensitivity * 1.27f);
		cache[t].pitchsustain = cache[t].pitchEnv.level[3];

		// Calculate and cache TVA envelope stuff
		cache[t].ampEnv = timbre->partial[t].tva;
		cache[t].ampEnv.level = (char)((float)cache[t].ampEnv.level * 1.27f);

		int jr = 4;
		for (int jt = 3; jt >= 0; --jt) {
			if (cache[t].ampEnv.envLevel[jt] > 0) {
				break;
			}
			jr = jt;
		}
		cache[t].ampDecayStep = jr;

		cache[t].ampbias[0] = fixBiaslevel(cache[t].ampEnv.biasPoint1, &cache[t].ampdir[0]);
		cache[t].ampblevel[0] = 12 - cache[t].ampEnv.biasLevel1;
		cache[t].ampbias[1] = fixBiaslevel(cache[t].ampEnv.biasPoint2, &cache[t].ampdir[1]);
		cache[t].ampblevel[1] = 12 - cache[t].ampEnv.biasLevel2;
		cache[t].ampdepth = cache[t].ampEnv.envTimeVeloSensitivity * cache[t].ampEnv.envTimeVeloSensitivity;

		// Calculate and cache filter stuff
		cache[t].filtEnv = timbre->partial[t].tvf;
		cache[t].filtkeyfollow  = fixKeyfollow(cache[t].filtEnv.keyfollow);
		cache[t].filtEnv.envDepth = (char)((float)cache[t].filtEnv.envDepth);
		cache[t].tvfbias = fixBiaslevel(cache[t].filtEnv.biasPoint, &cache[t].tvfdir);
		cache[t].tvfblevel = cache[t].filtEnv.biasLevel;
		cache[t].filtsustain  = cache[t].filtEnv.envLevel[3];

		// Calculate and cache LFO stuff
		cache[t].lfodepth = timbre->partial[t].pitchLFO.depth;
		cache[t].lfoperiod = synth->tables.lfoPeriod[(int)timbre->partial[t].pitchLFO.rate];
		cache[t].lforate = timbre->partial[t].pitchLFO.rate;
		cache[t].modsense = timbre->partial[t].pitchLFO.modSensitivity;
	}
	for (int t = 0; t < 4; t++) {
		// Common parameters, stored redundantly
		cache[t].dirty = false;
		cache[t].partialCount = partialCount;
		cache[t].sustain = (timbre->common.noSustain == 0);
	}
	//synth->printDebug("Res 1: %d 2: %d 3: %d 4: %d", cache[0].waveform, cache[1].waveform, cache[2].waveform, cache[3].waveform);

#if MT32EMU_MONITOR_INSTRUMENTS == 1
	synth->printDebug("%s (%s): Recached timbre", name, currentInstr);
	for (int i = 0; i < 4; i++) {
		synth->printDebug(" %d: play=%s, pcm=%s (%d), wave=%d", i, cache[i].playPartial ? "YES" : "NO", cache[i].PCMPartial ? "YES" : "NO", timbre->partial[i].wg.pcmWave, timbre->partial[i].wg.waveform);
	}
#endif
}

const char *Part::getName() const {
	return name;
}

void Part::updateVolume() {
	// DEPRECATED: This method will soon be obsolete and removed
	volumeMult = (patchTemp->outputLevel * midiExpression) / 127;
}

int Part::getVolume() const {
	// FIXME: Use the mappings for this in the control ROM
	return patchTemp->outputLevel * 127 / 100;
}

void Part::setVolume(int midiVolume) {
	// CONFIRMED: This calculation matches the table used in the control ROM
	patchTemp->outputLevel = (Bit8u)(midiVolume * 100 / 127);
	updateVolume();
	//synth->printDebug("%s (%s): Set volume to %d", name, currentInstr, midiVolume);
}

Bit8u Part::getExpression() const {
	return expression;
}

void Part::setExpression(int midiExpression) {
	// CONFIRMED: This calculation matches the table used in the control ROM
	expression = (Bit8u)(midiExpression * 100 / 127);

	// DEPRECATED: This will become obsolete and go away soon
	this->midiExpression = midiExpression;
	updateVolume();
}

void RhythmPart::setPan(unsigned int midiPan)
{
	// FIXME:KG: This is unchangeable for drums (they always use drumPan), is that correct?
	synth->printDebug("%s: Setting pan (%d) not supported on rhythm", name, midiPan);
}

void Part::setPan(unsigned int midiPan) {
	// FIXME:KG: Tweaked this a bit so that we have a left 100%, centre and right 100%
	// (But this makes the range somewhat skewed)
	// Check against the real thing
	// NOTE: Panning is inverted compared to GM.
	if (midiPan < 64) {
		volumesetting.leftvol = (Bit16s)(midiPan * 512);
		volumesetting.rightvol = 32767;
	} else if (midiPan == 64) {
		volumesetting.leftvol = 32767;
		volumesetting.rightvol = 32767;
	} else {
		volumesetting.rightvol = (Bit16s)((127 - midiPan) * 520);
		volumesetting.leftvol = 32767;
	}
	patchTemp->panpot = (Bit8u)(midiPan * 14 / 127);
	//synth->printDebug("%s (%s): Set pan to %d", name, currentInstr, panpot);
}

void RhythmPart::playNote(unsigned int key, int vel) {
	if (key < 24 || key > 108)/*> 87 on MT-32)*/ {
		synth->printDebug("%s: Attempted to play invalid key %d", name, key);
		return;
	}
	int drumNum = key - 24;
	int drumTimbreNum = rhythmTemp[drumNum].timbre;
	if (drumTimbreNum >= 127) { // 94 on MT-32
		synth->printDebug("%s: Attempted to play unmapped key %d", name, key);
		return;
	}
	int absTimbreNum = drumTimbreNum + 128;
	TimbreParam *timbre = &synth->mt32ram.timbres[absTimbreNum].timbre;
	memcpy(currentInstr, timbre->common.name, 10);
#if MT32EMU_MONITOR_INSTRUMENTS == 1
	synth->printDebug("%s (%s): starting poly (drum %d, timbre %d) - Vel %d Key %d", name, currentInstr, drumNum, absTimbreNum, vel, key);
#endif
	if (drumCache[drumNum][0].dirty) {
		cacheTimbre(drumCache[drumNum], timbre);
	}
	playPoly(drumCache[drumNum], key, MIDDLEC, vel);
}

void Part::playNote(unsigned int key, int vel) {
	int freqNum = key;
	if (freqNum < 12) {
		synth->printDebug("%s (%s): Attempted to play invalid key %d < 12; moving up by octave", name, currentInstr, key);
		freqNum += 12;
	} else if (freqNum > 108) {
		synth->printDebug("%s (%s): Attempted to play invalid key %d > 108; moving down by octave", name, currentInstr, key);
		while (freqNum > 108) {
			freqNum -= 12;
		}
	}
	// POLY1 mode, Single Assign
	// Haven't found any software that uses any of the other poly modes
	// FIXME:KG: Should this also apply to rhythm?
	for (unsigned int i = 0; i < MT32EMU_MAX_POLY; i++) {
		if (polyTable[i].isActive() && (polyTable[i].key == key)) {
			//AbortPoly(&polyTable[i]);
			stopNote(key);
			break;
		}
	}
#if MT32EMU_MONITOR_INSTRUMENTS == 1
	synth->printDebug("%s (%s): starting poly - Vel %d Key %d", name, currentInstr, vel, key);
#endif
	if (patchCache[0].dirty) {
		cacheTimbre(patchCache, timbreTemp);
	}
	playPoly(patchCache, key, freqNum, vel);
}

void Part::playPoly(const PatchCache cache[4], unsigned int key, int freqNum, int vel) {
	unsigned int needPartials = cache[0].partialCount;
	unsigned int freePartials = synth->partialManager->getFreePartialCount();

	if (freePartials < needPartials) {
		if (!synth->partialManager->freePartials(needPartials - freePartials, partNum)) {
			synth->printDebug("%s (%s): Insufficient free partials to play key %d (vel=%d); needed=%d, free=%d", name, currentInstr, key, vel, needPartials, synth->partialManager->getFreePartialCount());
			return;
		}
	}
	// Find free poly
	int m;
	for (m = 0; m < MT32EMU_MAX_POLY; m++) {
		if (!polyTable[m].isActive()) {
			break;
		}
	}
	if (m == MT32EMU_MAX_POLY) {
		synth->printDebug("%s (%s): No free poly to play key %d (vel %d)", name, currentInstr, key, vel);
		return;
	}

	Poly *tpoly = &polyTable[m];

	tpoly->isPlaying = true;
	tpoly->key = key;
	tpoly->isDecay = false;
	tpoly->freqnum = freqNum;
	tpoly->vel = vel;
	tpoly->pedalhold = false;

	bool allnull = true;
	for (int x = 0; x < 4; x++) {
		if (cache[x].playPartial) {
			tpoly->partials[x] = synth->partialManager->allocPartial(partNum);
			allnull = false;
		} else {
			tpoly->partials[x] = NULL;
		}
	}

	if (allnull)
		synth->printDebug("%s (%s): No partials to play for this instrument", name, this->currentInstr);

	tpoly->sustain = cache[0].sustain;
	tpoly->volumeptr = &volumeMult;

	for (int x = 0; x < 4; x++) {
		if (tpoly->partials[x] != NULL) {
			tpoly->partials[x]->startPartial(tpoly, &cache[x], tpoly->partials[cache[x].structurePair]);
			tpoly->partials[x]->setBend(bend);
		}
	}
}

void Part::allNotesOff() {
	// Note: Unchecked on real MT-32, but the MIDI specification states that all notes off (0x7B)
	// should treat the hold pedal as usual.
	// All *sound* off (0x78) should stop notes immediately regardless of the hold pedal.
	// The latter controller is not implemented on the MT-32 (according to the docs).
	for (int q = 0; q < MT32EMU_MAX_POLY; q++) {
		Poly *tpoly = &polyTable[q];
		if (tpoly->isPlaying) {
			if (holdpedal)
				tpoly->pedalhold = true;
			else if (tpoly->sustain)
				tpoly->startDecay();
		}
	}
}

void Part::allSoundOff() {
	for (int q = 0; q < MT32EMU_MAX_POLY; q++) {
		Poly *tpoly = &polyTable[q];
		if (tpoly->isPlaying) {
			tpoly->startDecay();
		}
	}
}

void Part::stopPedalHold() {
	for (int q = 0; q < MT32EMU_MAX_POLY; q++) {
		Poly *tpoly;
		tpoly = &polyTable[q];
		if (tpoly->isActive() && tpoly->pedalhold)
			stopNote(tpoly->key);
	}
}

void Part::stopNote(unsigned int key) {
	// Non-sustaining instruments ignore stop commands.
	// They die away eventually anyway

#if MT32EMU_MONITOR_INSTRUMENTS == 1
	synth->printDebug("%s (%s): stopping key %d", name, currentInstr, key);
#endif

	if (key != 255) {
		for (int q = 0; q < MT32EMU_MAX_POLY; q++) {
			Poly *tpoly = &polyTable[q];
			if (tpoly->isPlaying && tpoly->key == key) {
				if (holdpedal)
					tpoly->pedalhold = true;
				else if (tpoly->sustain)
					tpoly->startDecay();
			}
		}
		return;
	}

	// Find oldest poly... yes, the MT-32 can be reconfigured to kill different poly first
	// This is simplest
	int oldest = -1;
	Bit32u oldage = 0;

	for (int q = 0; q < MT32EMU_MAX_POLY; q++) {
		Poly *tpoly = &polyTable[q];

		if (tpoly->isPlaying && !tpoly->isDecay) {
			if (tpoly->getAge() >= oldage) {
				oldage = tpoly->getAge();
				oldest = q;
			}
		}
	}

	if (oldest != -1) {
		polyTable[oldest].startDecay();
	}
}

const MemParams::PatchTemp *Part::getPatchTemp() const {
	return patchTemp;
}

const MemParams::RhythmTemp *Part::getRhythmTemp() const {
	return NULL;
}

const MemParams::RhythmTemp *RhythmPart::getRhythmTemp() const {
	return rhythmTemp;
}

}
