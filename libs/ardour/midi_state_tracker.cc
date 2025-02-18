/*
 * Copyright (C) 2008-2016 David Robillard <d@drobilla.net>
 * Copyright (C) 2009-2017 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2010-2012 Carl Hetherington <carl@carlh.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <iostream>

#include "pbd/compose.h"

#include "evoral/EventSink.h"

#include "ardour/debug.h"
#include "ardour/midi_source.h"
#include "ardour/midi_state_tracker.h"
#include "ardour/parameter_types.h"

using namespace std;
using namespace ARDOUR;


MidiNoteTracker::MidiNoteTracker ()
{
	reset ();
}

void
MidiNoteTracker::reset ()
{
	DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1: reset\n", this));
	memset (_active_notes, 0, sizeof (_active_notes));
	_on = 0;
}

void
MidiNoteTracker::add (uint8_t note, uint8_t chn)
{
	if (_active_notes[note+128 * chn] == 0) {
		++_on;
	}
	++_active_notes[note + 128 * chn];

	if (_active_notes[note+128 * chn] > 1) {
		//cerr << this << " note " << (int) note << '/' << (int) chn << " was already on, now at " << (int) _active_notes[note+128*chn] << endl;
	}

	DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1 ON %2/%3 voices %5 total on %4\n",
							       this, (int) note, (int) chn, _on,
							       (int) _active_notes[note+128 * chn]));
}

void
MidiNoteTracker::remove (uint8_t note, uint8_t chn)
{
	switch (_active_notes[note + 128 * chn]) {
	case 0:
		break;
	case 1:
		--_on;
		_active_notes [note + 128 * chn] = 0;
		break;
	default:
		--_active_notes [note + 128 * chn];
		break;

	}
	DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1 OFF %2/%3 current voices = %5 total on %4\n",
							       this, (int) note, (int) chn, _on,
							       (int) _active_notes[note+128 * chn]));
}

void
MidiNoteTracker::track (const MidiBuffer::const_iterator &from, const MidiBuffer::const_iterator &to)
{
	for (MidiBuffer::const_iterator i = from; i != to; ++i) {
		track(*i);
	}
}

void
MidiNoteTracker::track (const uint8_t* evbuf)
{
	const uint8_t type = evbuf[0] & 0xF0;
	const uint8_t chan = evbuf[0] & 0x0F;
	switch (type) {
	case MIDI_CTL_ALL_NOTES_OFF:
		reset();
		break;
	case MIDI_CMD_NOTE_ON:
		add(evbuf[1], chan);
		break;
	case MIDI_CMD_NOTE_OFF:
		remove(evbuf[1], chan);
		break;
	}
}

void
MidiNoteTracker::resolve_notes (MidiBuffer &dst, samplepos_t time)
{
	DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1 MB-resolve notes @ %2 on = %3\n", this, time, _on));

	if (!_on) {
		return;
	}

	for (int channel = 0; channel < 16; ++channel) {
		for (int note = 0; note < 128; ++note) {
			while (_active_notes[note + 128 * channel]) {
				uint8_t buffer[3] = { ((uint8_t) (MIDI_CMD_NOTE_OFF | channel)), uint8_t (note), 0 };
				Evoral::Event<MidiBuffer::TimeType> noteoff
					(Evoral::MIDI_EVENT, time, 3, buffer, false);
				/* note that we do not care about failure from
				   push_back() ... should we warn someone ?
				*/
				dst.push_back (noteoff);
				_active_notes[note + 128 * channel]--;
				DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1: MB-resolved note %2/%3 at %4\n",
										       this, (int) note, (int) channel, time));
			}
		}
	}
	_on = 0;
}

void
MidiNoteTracker::resolve_notes (Evoral::EventSink<samplepos_t> &dst, samplepos_t time)
{
	uint8_t buf[3];

	DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1 EVS-resolve notes @ %2 on = %3\n", this, time, _on));

	if (!_on) {
		return;
	}

	for (int channel = 0; channel < 16; ++channel) {
		for (int note = 0; note < 128; ++note) {
			while (_active_notes[note + 128 * channel]) {
				buf[0] = MIDI_CMD_NOTE_OFF|channel;
				buf[1] = note;
				buf[2] = 0;
				/* note that we do not care about failure from
				   write() ... should we warn someone ?
				*/
				dst.write (time, Evoral::MIDI_EVENT, 3, buf);
				_active_notes[note + 128 * channel]--;
				DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1: EVS-resolved note %2/%3 at %4\n",
										       this, (int) note, (int) channel, time));
			}
		}
	}
	_on = 0;
}

void
MidiNoteTracker::resolve_notes (MidiSource& src, const MidiSource::Lock& lock, Temporal::Beats time)
{
	DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1 MS-resolve notes @ %2 on = %3\n", this, time, _on));

	if (!_on) {
		return;
	}

	/* NOTE: the src must be locked */

	for (int channel = 0; channel < 16; ++channel) {
		for (int note = 0; note < 128; ++note) {
			while (_active_notes[note + 128 * channel]) {
				Evoral::Event<Temporal::Beats> ev (Evoral::MIDI_EVENT, time, 3, 0, true);
				ev.set_type (MIDI_CMD_NOTE_OFF);
				ev.set_channel (channel);
				ev.set_note (note);
				ev.set_velocity (0);
				src.append_event_beats (lock, ev);
				DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1: MS-resolved note %2/%3 at %4\n",
										       this, (int) note, (int) channel, time));
				_active_notes[note + 128 * channel]--;
				/* don't stack events up at the same time */
				time += Temporal::Beats::one_tick();
			}
		}
	}
	_on = 0;
}

void
MidiNoteTracker::flush_notes (MidiBuffer &dst, samplepos_t time)
{
	DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1 MB-flushing notes @ %2 on = %3\n", this, time, _on));

	if (!_on) {
		return;
	}

	for (int channel = 0; channel < 16; ++channel) {
		for (int note = 0; note < 128; ++note) {
			while (_active_notes[note + 128 * channel]) {
				uint8_t buffer[3] = { ((uint8_t) (MIDI_CMD_NOTE_ON | channel)), uint8_t (note), 0 };
				Evoral::Event<MidiBuffer::TimeType> noteoff (Evoral::MIDI_EVENT, time, 3, buffer, false);
				/* note that we do not care about failure from
				   push_back() ... should we warn someone ?
				*/
				dst.push_back (noteoff);
				_active_notes[note + 128 * channel]--;
				DEBUG_TRACE (PBD::DEBUG::MidiTrackers, string_compose ("%1: MB-flushed note %2/%3 at %4\n",
										       this, (int) note, (int) channel, time));
			}
		}
	}
	_on = 0;
}

void
MidiNoteTracker::dump (ostream& o)
{
	o << "******\n";
	for (int c = 0; c < 16; ++c) {
		for (int x = 0; x < 128; ++x) {
			if (_active_notes[c * 128 + x]) {
				o << "Channel " << c+1 << " Note " << x << " is on ("
				  << (int) _active_notes[c*128+x] <<  " times)\n";
			}
		}
	}
	o << "+++++\n";
}

/*----------------*/

MidiStateTracker::MidiStateTracker ()
{
	reset ();
}

void
MidiStateTracker::reset ()
{
	const size_t n_channels = 16;
	const size_t n_controls = 127;

	MidiNoteTracker::reset ();

	for (size_t n = 0; n < n_channels; ++n) {
		program[n] = 0x80;
	}

	for (size_t chn = 0; chn < n_channels; ++chn) {
		for (size_t c = 0; c < n_controls; ++c) {
			control[chn][c] = 0x80;
		}
	}
}

void
MidiStateTracker::dump (ostream& o)
{
	MidiNoteTracker::dump (o);
	o << "implement MidiStateTracker::dump()";
}

void
MidiStateTracker::track (const uint8_t* evbuf)
{
	const uint8_t type = evbuf[0] & 0xF0;
	const uint8_t chan = evbuf[0] & 0x0F;

	switch (type) {
	case MIDI_CTL_ALL_NOTES_OFF:
		MidiNoteTracker::reset();
		break;

	case MIDI_CMD_NOTE_ON:
		add (evbuf[1], chan);
		break;
	case MIDI_CMD_NOTE_OFF:
		remove (evbuf[1], chan);
		break;

	case MIDI_CMD_CONTROL:
		control[chan][evbuf[1]] = evbuf[2];
		break;

	case MIDI_CMD_PGM_CHANGE:
		program[chan] = evbuf[1];
		break;

	case MIDI_CMD_CHANNEL_PRESSURE:
		pressure[chan] = evbuf[1];
		break;

	case MIDI_CMD_NOTE_PRESSURE:
		break;

	case MIDI_CMD_BENDER:
		break;

	case MIDI_CMD_COMMON_RESET:
		reset ();
		break;

	default:
 		break;
	}
}

void
MidiStateTracker::flush (MidiBuffer& dst, samplepos_t time)
{
	/* XXX implement me */

	uint8_t buf[3];
	const size_t n_channels = 16;
	const size_t n_controls = 127;

	/* XXX need MidiNoteTracker::flush() method that will emit NoteOn for
	 * all currently-on notes.
	 */

	for (int chn = 0; chn < n_channels; ++chn) {
		if (program[chn] & 0x80 == 0) {
			buf[0] = MIDI_CMD_PGM_CHANGE|chn;
			buf[1] = program[chn] & 0x7f;
			dst.write (time, Evoral::MIDI_EVENT, 2, buf);
		}
	}

	for (int chn = 0; chn < n_channels; ++chn) {
		for (int ctl = 0; ctl < n_controls; ++ctl) {
			if (control[chn][ctl] & 0x80 == 0) {
				buf[0] = MIDI_CMD_CONTROL|chn;
				buf[1] = ctl;
				buf[2] = control[chn][ctl] & 0x7f;
				dst.write (time, Evoral::MIDI_EVENT, 3, buf);
			}
		}
	}
}
