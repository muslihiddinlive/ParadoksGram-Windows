/*
This file is part of ParadoksGram Windows,
a modified Telegram Desktop client.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "paradoks/paradoks_voice.h"

#include "paradoks/paradoks_config.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

namespace Paradoks::Voice {
namespace {

constexpr auto kPi = 3.14159265358979323846;

class Biquad {
public:
	[[nodiscard]] static Biquad Peaking(
			int rate,
			float freq,
			float q,
			float gainDb) {
		const auto A = std::pow(10., gainDb / 40.);
		const auto w0 = 2. * kPi * freq / rate;
		const auto alpha = std::sin(w0) / (2. * q);
		const auto cosW = std::cos(w0);
		const auto a0inv = 1. / (1. + alpha / A);
		return Biquad(
			(1. + alpha * A) * a0inv,
			(-2. * cosW) * a0inv,
			(1. - alpha * A) * a0inv,
			(-2. * cosW) * a0inv,
			(1. - alpha / A) * a0inv);
	}

	[[nodiscard]] static Biquad BandPass(int rate, float freq, float q) {
		const auto w0 = 2. * kPi * freq / rate;
		const auto alpha = std::sin(w0) / (2. * q);
		const auto cosW = std::cos(w0);
		const auto a0inv = 1. / (1. + alpha);
		return Biquad(
			alpha * a0inv,
			0.,
			-alpha * a0inv,
			(-2. * cosW) * a0inv,
			(1. - alpha) * a0inv);
	}

	// RBJ audio-EQ-cookbook high shelf with slope S = 1.
	[[nodiscard]] static Biquad HighShelf(
			int rate,
			float freq,
			float gainDb) {
		const auto A = std::pow(10., gainDb / 40.);
		const auto w0 = 2. * kPi * freq / rate;
		const auto cosW = std::cos(w0);
		const auto sinW = std::sin(w0);
		const auto alpha = sinW / std::sqrt(2.);
		const auto beta = 2. * std::sqrt(A) * alpha;
		const auto a0inv = 1.
			/ ((A + 1.) - (A - 1.) * cosW + beta);
		return Biquad(
			A * ((A + 1.) + (A - 1.) * cosW + beta) * a0inv,
			-2. * A * ((A - 1.) + (A + 1.) * cosW) * a0inv,
			A * ((A + 1.) + (A - 1.) * cosW - beta) * a0inv,
			2. * ((A - 1.) - (A + 1.) * cosW) * a0inv,
			((A + 1.) - (A - 1.) * cosW - beta) * a0inv);
	}

	[[nodiscard]] float process(float x) {
		const auto y = _b0 * x + _b1 * _x1 + _b2 * _x2
			- _a1 * _y1 - _a2 * _y2;
		_x2 = _x1;
		_x1 = x;
		_y2 = _y1;
		_y1 = y;
		return y;
	}

private:
	Biquad(double b0, double b1, double b2, double a1, double a2)
	: _b0(float(b0))
	, _b1(float(b1))
	, _b2(float(b2))
	, _a1(float(a1))
	, _a2(float(a2)) {
	}

	float _b0 = 0.f;
	float _b1 = 0.f;
	float _b2 = 0.f;
	float _a1 = 0.f;
	float _a2 = 0.f;
	float _x1 = 0.f;
	float _x2 = 0.f;
	float _y1 = 0.f;
	float _y2 = 0.f;

};

class Effect {
public:
	virtual ~Effect() = default;
	[[nodiscard]] virtual float process(float x) = 0;
};

// Ring modulation with a 120 Hz saw plus two band-passed copies.
class Robot final : public Effect {
public:
	explicit Robot(int rate)
	: _phaseInc(kBuzzHz / float(rate))
	, _bp1(Biquad::BandPass(rate, 600.f, 3.f))
	, _bp2(Biquad::BandPass(rate, 1500.f, 2.5f))
	, _lpCoef(float(1. - std::exp(-2. * kPi * 3500. / rate))) {
	}

	float process(float x) override {
		_phase += _phaseInc;
		if (_phase >= 1.f) {
			_phase -= 1.f;
		}
		const auto buzz = 2.f * _phase - 1.f;
		const auto ring = x * buzz;
		const auto shaped = 0.55f * ring
			+ 0.5f * (_bp1.process(ring) + _bp2.process(ring));
		_lp += _lpCoef * (shaped - _lp);
		return _lp * kMakeup;
	}

private:
	static constexpr auto kBuzzHz = 120.f;
	static constexpr auto kMakeup = 2.f;

	float _phaseInc = 0.f;
	float _phase = 0.f;
	Biquad _bp1;
	Biquad _bp2;
	float _lp = 0.f;
	float _lpCoef = 0.f;

};

// Freeverb-style Schroeder reverb (4 combs, 2 all-pass) with a long echo.
class Cave final : public Effect {
public:
	explicit Cave(int rate) {
		const int delays[] = {
			Scale(1557, rate),
			Scale(1617, rate),
			Scale(1491, rate),
			Scale(1422, rate),
		};
		for (const auto delay : delays) {
			_combs.emplace_back(std::max(2, delay), 0.80f, 0.22f);
		}
		_aps.emplace_back(std::max(2, Scale(225, rate)), 0.5f);
		_aps.emplace_back(std::max(2, Scale(556, rate)), 0.5f);
		_echo.assign(std::max(2, rate * 320 / 1000), 0.f);
		_echoLp = float(1. - std::exp(-2. * kPi * 3000. / rate));
	}

	float process(float x) override {
		auto wet = 0.f;
		for (auto &comb : _combs) {
			wet += comb.process(x);
		}
		wet *= 0.25f;
		for (auto &ap : _aps) {
			wet = ap.process(wet);
		}

		const auto e = _echo[_echoPos];
		_echoFeed += _echoLp * (e - _echoFeed);
		_echo[_echoPos] = x + 0.35f * _echoFeed;
		if (++_echoPos >= int(_echo.size())) {
			_echoPos = 0;
		}
		return 0.60f * x + 0.50f * wet + 0.30f * _echoFeed;
	}

private:
	class Comb {
	public:
		Comb(int size, float feedback, float damp)
		: _buffer(size, 0.f)
		, _feedback(feedback)
		, _damp(damp) {
		}

		float process(float x) {
			const auto out = _buffer[_pos];
			_store = out * (1.f - _damp) + _store * _damp;
			_buffer[_pos] = x + _store * _feedback;
			if (++_pos >= int(_buffer.size())) {
				_pos = 0;
			}
			return out;
		}

	private:
		std::vector<float> _buffer;
		int _pos = 0;
		float _feedback = 0.f;
		float _damp = 0.f;
		float _store = 0.f;

	};

	class AllPass {
	public:
		AllPass(int size, float gain)
		: _buffer(size, 0.f)
		, _gain(gain) {
		}

		float process(float x) {
			const auto d = _buffer[_pos];
			const auto out = d - _gain * x;
			_buffer[_pos] = x + _gain * d;
			if (++_pos >= int(_buffer.size())) {
				_pos = 0;
			}
			return out;
		}

	private:
		std::vector<float> _buffer;
		int _pos = 0;
		float _gain = 0.f;

	};

	[[nodiscard]] static int Scale(int samples44k, int rate) {
		return int(int64(samples44k) * rate / 44100);
	}

	std::vector<Comb> _combs;
	std::vector<AllPass> _aps;
	std::vector<float> _echo;
	int _echoPos = 0;
	float _echoFeed = 0.f;
	float _echoLp = 0.f;

};

// Minecraft villager: +3 semitones with a two-tap crossfading delay-line
// pitch shifter, a nasal peak at 800 Hz and a gentle high shelf cut.
class Villager final : public Effect {
public:
	explicit Villager(int rate)
	: _window(rate * 0.020f)
	, _phaseInc(float(kRatio - 1.) / (rate * 0.020f))
	, _nasal(Biquad::Peaking(rate, 800.f, 2.f, 5.f))
	, _shelf(Biquad::HighShelf(rate, 5000.f, -6.f)) {
		auto size = 1;
		while (size < _window + 16.f) {
			size <<= 1;
		}
		_buffer.assign(size, 0.f);
		_mask = size - 1;
	}

	float process(float x) override {
		_buffer[_write] = x;

		const auto pA = _phase;
		auto pB = _phase + 0.5f;
		if (pB >= 1.f) {
			pB -= 1.f;
		}
		const auto sA = float(std::sin(kPi * pA));
		const auto sB = float(std::sin(kPi * pB));
		const auto s = tap(pA) * (sA * sA) + tap(pB) * (sB * sB);

		_phase += _phaseInc;
		if (_phase >= 1.f) {
			_phase -= 1.f;
		}
		_write = (_write + 1) & _mask;

		return _shelf.process(_nasal.process(s));
	}

private:
	// 2^(3/12): three semitones up.
	static constexpr auto kRatio = 1.189207115002721;

	// Catmull-Rom read at a delay of 2 .. 2 + window samples.
	[[nodiscard]] float tap(float p) const {
		const auto pos = float(_write) - (2.f + _window * (1.f - p));
		const auto base = std::floor(pos);
		const auto i = int(base);
		const auto f = pos - base;
		const auto at = [&](int k) {
			return _buffer[unsigned(k) & unsigned(_mask)];
		};
		const auto y0 = at(i - 1);
		const auto y1 = at(i);
		const auto y2 = at(i + 1);
		const auto y3 = at(i + 2);
		const auto c1 = 0.5f * (y2 - y0);
		const auto c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
		const auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
		return ((c3 * f + c2) * f + c1) * f + y1;
	}

	std::vector<float> _buffer;
	int _mask = 0;
	int _write = 0;
	float _window = 0.f;
	float _phaseInc = 0.f;
	float _phase = 0.f;
	Biquad _nasal;
	Biquad _shelf;

};

struct State {
	std::mutex mutex;
	std::unique_ptr<Effect> effect;
	Mode mode = Mode::Off;
	int rate = 0;
};

[[nodiscard]] State &Current() {
	static auto state = State();
	return state;
}

[[nodiscard]] std::unique_ptr<Effect> Create(Mode mode, int rate) {
	switch (mode) {
	case Mode::Robot: return std::make_unique<Robot>(rate);
	case Mode::Cave: return std::make_unique<Cave>(rate);
	case Mode::Villager: return std::make_unique<Villager>(rate);
	case Mode::Off: break;
	}
	return nullptr;
}

// Gentle cubic soft clipper: transparent for small signals.
[[nodiscard]] float SoftClip(float y) {
	if (y >= 1.5f) {
		return 1.f;
	} else if (y <= -1.5f) {
		return -1.f;
	}
	return y - 0.148148148f * y * y * y;
}

} // namespace

Mode CurrentMode() {
	return Mode(std::clamp(VoiceEffect(), 0, int(Mode::Villager)));
}

void SetMode(Mode mode) {
	SetVoiceEffect(int(mode));
}

QString ModeTitle(Mode mode) {
	switch (mode) {
	case Mode::Robot: return u"Robot"_q;
	case Mode::Cave: return u"G'or"_q;
	case Mode::Villager: return u"Minecraft Villager"_q;
	case Mode::Off: break;
	}
	return u"O'chiq"_q;
}

void Reset() {
	auto &state = Current();
	const auto guard = std::lock_guard(state.mutex);
	state.effect = nullptr;
	state.mode = Mode::Off;
	state.rate = 0;
}

void Process(short *samples, int count, int sampleRate) {
	const auto mode = CurrentMode();
	if (mode == Mode::Off || !samples || count <= 0) {
		return;
	}
	auto &state = Current();
	const auto guard = std::lock_guard(state.mutex);
	if (!state.effect || state.mode != mode || state.rate != sampleRate) {
		state.effect = Create(mode, sampleRate);
		state.mode = mode;
		state.rate = sampleRate;
	}
	if (!state.effect) {
		return;
	}
	for (auto i = 0; i != count; ++i) {
		const auto x = float(samples[i]) / 32768.f;
		const auto y = SoftClip(state.effect->process(x));
		samples[i] = short(std::lround(y * 32767.f));
	}
}

} // namespace Paradoks::Voice
