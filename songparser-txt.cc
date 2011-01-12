#include "songparser.hh"

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <stdexcept>
#include <iostream>

/// @file
/// Functions used for parsing the UltraStar TXT song format

using namespace SongParserUtil;

/// 'Magick' to check if this file looks like correct format
bool SongParser::txtCheck(std::vector<char> const& data) {
	return data[0] == '#' && data[1] >= 'A' && data[1] <= 'Z';
}

/// Parse header data for Songs screen
void SongParser::txtParseHeader() {
	Song& s = m_song;
	std::string line;
	while (getline(line) && txtParseField(line)) {}
	if (s.title.empty() || s.artist.empty()) throw std::runtime_error("Required header fields missing");
	if (m_bpm != 0.0) addBPM(0, m_bpm);
	s.insertVocalTrack(TrackName::LEAD_VOCAL, VocalTrack(TrackName::LEAD_VOCAL)); // Dummy note to indicate there is a track
}

/// Parse notes
void SongParser::txtParse() {
	std::string line;
	VocalTrack vocal(TrackName::LEAD_VOCAL);
	while (getline(line) && txtParseField(line)) {} // Parse the header again
	if (m_bpm != 0.0) addBPM(0, m_bpm);
	while (txtParseNote(line, vocal) && getline(line)) {} // Parse notes
	// Workaround for the terminating : 1 0 0 line, written by some converters
	if (!vocal.notes.empty() && vocal.notes.back().type != Note::SLEEP
	  && vocal.notes.back().begin == vocal.notes.back().end) vocal.notes.pop_back();
	m_song.insertVocalTrack(TrackName::LEAD_VOCAL, vocal);
}

bool SongParser::txtParseField(std::string const& line) {
	if (line.empty()) return true;
	if (line[0] != '#') return false;
	std::string::size_type pos = line.find(':');
	if (pos == std::string::npos) throw std::runtime_error("Invalid txt format, should be #key:value");
	std::string key = boost::trim_copy(line.substr(1, pos - 1));
	std::string value = boost::trim_copy(line.substr(pos + 1));
	if (value.empty()) return true;
	if (key == "TITLE") m_song.title = value.substr(value.find_first_not_of(" :"));
	else if (key == "ARTIST") m_song.artist = value.substr(value.find_first_not_of(" "));
	else if (key == "EDITION") m_song.edition = value.substr(value.find_first_not_of(" "));
	else if (key == "GENRE") m_song.genre = value.substr(value.find_first_not_of(" "));
	else if (key == "CREATOR") m_song.creator = value.substr(value.find_first_not_of(" "));
	else if (key == "COVER") m_song.cover = value;
	else if (key == "MP3") m_song.music["background"] = m_song.path + value;
	else if (key == "VOCALS") m_song.music["vocals"] = m_song.path + value;
	else if (key == "VIDEO") m_song.video = value;
	else if (key == "BACKGROUND") m_song.background = value;
	else if (key == "START") assign(m_song.start, value);
	else if (key == "VIDEOGAP") assign(m_song.videoGap, value);
	else if (key == "PREVIEWSTART") assign(m_song.preview_start, value);
	else if (key == "RELATIVE") assign(m_relative, value);
	else if (key == "GAP") { assign(m_gap, value); m_gap *= 1e-3; }
	else if (key == "BPM") assign(m_bpm, value);
	else if (key == "LANGUAGE") m_song.language= value.substr(value.find_first_not_of(" "));
	return true;
}

bool SongParser::txtParseNote(std::string line, VocalTrack &vocal) {
	if (line.empty() || line == "\r") return true;
	if (line[0] == '#') throw std::runtime_error("Key found in the middle of notes");
	if (line[line.size() - 1] == '\r') line.erase(line.size() - 1);
	if (line[0] == 'E') return false;
	std::istringstream iss(line);
	if (line[0] == 'B') {
		unsigned int ts;
		double bpm;
		iss.ignore();
		if (!(iss >> ts >> bpm)) throw std::runtime_error("Invalid BPM line format");
		addBPM(ts, bpm);
		return true;
	}
	if (line[0] == 'P') return true; //We ignore player information for now (multiplayer hack)
	Note n;
	n.type = Note::Type(iss.get());
	unsigned int ts = m_prevts;
	switch (n.type) {
	  case Note::NORMAL:
	  case Note::FREESTYLE:
	  case Note::GOLDEN:
		{
			unsigned int length = 0;
			if (!(iss >> ts >> length >> n.note)) throw std::runtime_error("Invalid note line format");
			n.notePrev = n.note; // No slide notes in TXT yet.
			if (m_relative) ts += m_relativeShift;
			if (iss.get() == ' ') std::getline(iss, n.syllable);
			n.end = tsTime(ts + length);
		}
		break;
	  case Note::SLEEP:
		{
			unsigned int end;
			if (!(iss >> ts >> end)) end = ts;
			if (m_relative) {
				ts += m_relativeShift;
				end += m_relativeShift;
				m_relativeShift = end;
			}
			n.end = tsTime(end);
		}
		break;
	  default: throw std::runtime_error("Unknown note type");
	}
	n.begin = tsTime(ts);
	Notes& notes = vocal.notes;
	if (m_relative && notes.empty()) m_relativeShift = ts;
	m_prevts = ts;
	if (n.begin < m_prevtime) {
		// Oh no, overlapping notes (b0rked file)
		// Can't do this because too many songs are b0rked: throw std::runtime_error("Note overlaps with previous note");
		if (notes.size() >= 1) {
			Note& p = notes.back();
			// Workaround for songs that use semi-random timestamps for sleep
			if (p.type == Note::SLEEP) {
				p.end = p.begin;
				Notes::reverse_iterator it = notes.rbegin();
				Note& p2 = *++it;
				if (p2.end < n.begin) p.begin = p.end = n.begin;
			}
			// Can we just make the previous note shorter?
			if (p.begin <= n.begin) p.end = n.begin;
			else { // Nothing to do, warn and skip
				std::ostringstream oss;
				oss << "Skipping overlapping note in " << m_song.path << m_song.filename << std::endl;
				std::cout << "songparser/warning: " << oss.str(); // More likely to be atomic when written as one string
				return true;
			}
		} else throw std::runtime_error("The first note has negative timestamp");
	}
	double prevtime = m_prevtime;
	m_prevtime = n.end;
	if (n.type != Note::SLEEP && n.end > n.begin) {
		vocal.noteMin = std::min(vocal.noteMin, n.note);
		vocal.noteMax = std::max(vocal.noteMax, n.note);
		//FIXME: m_maxScore += n.maxScore();
	}
	if (n.type == Note::SLEEP) {
		if (notes.empty()) return true; // Ignore sleeps at song beginning
		n.begin = n.end = prevtime; // Normalize sleep notes
		notes.back().lineBreak = true; // lineBreak flag for notes preceding SLEEPs
	}
	notes.push_back(n);
	return true;
}

