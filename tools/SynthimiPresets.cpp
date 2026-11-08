// Writes the default package's presets for CZumarzu and ViceVersa.
//
//   score_addon_synthimi_make_presets <Presets folder>
//
// Each preset is set on the processor's own ports, so the port indices in the
// .scp files come from the code and cannot drift from it; each control is
// also stored with its name, which the host matches first. Every preset
// stores every port: loading one sets only the ports it holds. Levels are
// matched by measurement: middle C, velocity 100, its loudest 100 ms brought
// to -24 dBFS RMS. A synth's folder is written anew: presets no longer in the
// list go. Exits non-zero if anything could not be written.
#include <CZumarzu/CZumarzu.hpp>
#include <ViceVersa/ViceVersa.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
//! A JSON string's contents
std::string escaped(std::string_view s)
{
  std::string out;
  for(char c : s)
  {
    switch(c)
    {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
        if(static_cast<unsigned char>(c) < 0x20)
        {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
          out += buf;
        }
        else
          out += c;
    }
  }
  return out;
}

void putFloat(std::ostream& o, double v)
{
  if(!std::isfinite(v))
    throw std::runtime_error("a control holds a value that is not finite");
  o << v;
}

template <typename T>
void put(std::ostream& o, const T& v)
{
  using V = std::remove_cvref_t<T>;
  if constexpr(std::is_enum_v<V>)
    o << "{\"Int\":" << int(v) << "}";
  else if constexpr(std::is_same_v<V, bool>)
    o << "{\"Bool\":" << (v ? "true" : "false") << "}";
  else if constexpr(std::is_integral_v<V>)
    o << "{\"Int\":" << v << "}";
  else if constexpr(std::is_floating_point_v<V>)
  {
    o << "{\"Float\":";
    putFloat(o, double(v));
    o << "}";
  }
  else if constexpr(requires { v.x; v.y; })
  {
    o << "{\"Vec2f\":[";
    putFloat(o, double(v.x));
    o << ",";
    putFloat(o, double(v.y));
    o << "]}";
  }
  else if constexpr(std::is_same_v<V, std::vector<float>>)
  {
    o << "{\"Tuple\":[";
    for(std::size_t i = 0; i < v.size(); i++)
    {
      o << (i ? "," : "") << "{\"Float\":";
      putFloat(o, double(v[i]));
      o << "}";
    }
    o << "]}";
  }
  else
    static_assert(sizeof(V) == 0, "port type without an encoding");
}

//! A port's value as the host stores it. A time chooser holds a pair: the
//! time, and 0 for seconds (else the synced fraction); a plain number would
//! be read as synced.
template <typename Port>
void putPort(std::ostream& o, const Port& port)
{
  if constexpr(requires { port.sync; })
  {
    if(port.sync)
      throw std::runtime_error("synced times are not written by this tool");
    o << "{\"Vec2f\":[";
    putFloat(o, double(port.value));
    o << ",0]}";
  }
  else
    put(o, port.value);
}

template <typename Proc>
std::string toJson(const std::string& name, const std::string& category, Proc& proc)
{
  std::ostringstream o;
  o.precision(std::numeric_limits<float>::max_digits10);
  o << "{\"Key\":{\"Uuid\":\"" << escaped(Proc::uuid()) << "\",\"Effect\":\"\"},\"Name\":\""
    << escaped(name) << "\",\"Category\":\"" << escaped(category) << "\",\"Preset\":[";
  int index = 0;
  bool first = true;
  avnd::input_introspection<Proc>::for_all(proc.inputs, [&](auto& port) {
    if constexpr(requires { port.value; })
    {
      o << (first ? "" : ",") << "[" << index << ",";
      putPort(o, port);
      o << ",\"" << escaped(std::remove_cvref_t<decltype(port)>::name()) << "\"]";
      first = false;
    }
    index++;
  });
  o << "]}";
  return o.str();
}

//! From silence: a change of rate resets everything (the same rate keeps
//! the voices, as a host's buffer change must)
template <typename Synth>
void fresh(Synth& s)
{
  s.prepare(44100.);
  s.prepare(48000.);
}

//! Loudest 100 ms of a held middle C over 8 s, RMS in dBFS: pads are
//! matched by their sustain, also after a slow attack, plucks and percussion
//! by their attack; one note, as mono presets play one anyway
template <typename Proc>
double measure(Proc& proc)
{
  auto& s = proc.synth;
  fresh(s);
  const auto p = proc.params();
  const unsigned char m[3]{0x90, 60, 100};
  s.midi(m, 3, p);
  constexpr int window = 4800;
  std::vector<float> l(window), r(window);
  double best = 0.;
  for(int w = 0; w < 80; w++)
  {
    s.render(p, l.data(), r.data(), window);
    double e = 0.;
    for(int k = 0; k < window; k++)
      e += 0.5 * (double(l[std::size_t(k)]) * l[std::size_t(k)]
                  + double(r[std::size_t(k)]) * r[std::size_t(k)]);
    best = std::max(best, e / window);
  }
  if(!std::isfinite(best))
    throw std::runtime_error("the synth's output is not finite");
  return 10. * std::log10(std::max(best, 1e-12));
}

/*! Where the notes of a rhythm start, in ms: rises of the 2 ms envelope by
 *  more than 6 dB in 10 ms, at least 40 ms apart */
template <typename Proc>
std::string onsets(Proc& proc)
{
  auto& s = proc.synth;
  fresh(s);
  const auto p = proc.params();
  const unsigned char m[3]{0x90, 60, 100};
  s.midi(m, 3, p);
  const int n = 48000;
  std::vector<float> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
  for(int i = 0; i < n; i += 256)
    s.render(p, l.data() + i, r.data() + i, std::min(256, n - i));
  std::vector<double> env(static_cast<std::size_t>(n));
  double e = 0.;
  for(int i = 0; i < n; i++)
  {
    e += (double(l[std::size_t(i)]) * l[std::size_t(i)] - e) * (1. / 96.);
    env[std::size_t(i)] = 10. * std::log10(e + 1e-12);
  }
  std::string out;
  int last = -100000;
  for(int i = 480; i < n; i++)
  {
    if(env[std::size_t(i)] - env[std::size_t(i - 480)] > 6. && i - last > 1920)
    {
      out += std::to_string(int(std::lround((i - 240) / 48.))) + " ";
      last = i;
    }
  }
  return out;
}

//! A synth's folder, emptied of the presets an earlier run wrote
std::filesystem::path presetFolder(const std::filesystem::path& root, const char* synth)
{
  const auto dir = root / synth;
  std::filesystem::create_directories(dir);
  for(const auto& f : std::filesystem::directory_iterator(dir))
    if(f.path().extension() == ".scp")
      std::filesystem::remove(f.path());
  return dir;
}

constexpr double kTargetDb = -24.;
constexpr float kMinLevel = 0.05f, kMaxLevel = 4.f; // the Level ports' range

template <typename Proc, typename Level>
void write(const std::filesystem::path& dir, const std::string& name,
           const std::string& category, Proc& proc, Level& level)
{
  if(name.empty() || name.find_first_of("/\\:") != std::string::npos)
    throw std::runtime_error("preset name not usable as a file name: " + name);
  // Three passes: the limiter makes the first corrections approximate
  for(int pass = 0; pass < 3; pass++)
  {
    const double db = measure(proc);
    level.value = float(std::clamp(
        double(level.value) * std::pow(10., (kTargetDb - db) / 20.), double(kMinLevel),
        double(kMaxLevel)));
  }
  const double db = measure(proc);
  const auto path = dir / (name + ".scp");
  std::ofstream f(path);
  f << toJson(name, category, proc);
  f.close();
  if(!f)
    throw std::runtime_error("could not write " + path.string());
  std::printf("%-24s %-10s level %.3f, %.1f dBFS\n", name.c_str(), category.c_str(),
              double(level.value), db);
  // Out of the level's range: the preset itself must be revoiced
  if(std::abs(db - kTargetDb) > 1.)
    std::fprintf(stderr, "warning: %s is at %.1f dBFS, level %.2f\n", name.c_str(), db,
                 double(level.value));
  if(category == "Rhythmic")
    std::printf("%-24s onsets (ms): %s\n", "", onsets(proc).c_str());
}

// ---- CZumarzu ---------------------------------------------------------------
using czumarzu::EnvSpec;
using czumarzu::SecondWave;
using czumarzu::Wave;

//! The rate whose full-scale move takes `s` seconds
float rate(double s)
{
  return float(std::log(s / czumarzu::kSlowest) / std::log(czumarzu::kFastest / czumarzu::kSlowest));
}

//! Steps as (seconds for a full-scale move, level)
EnvSpec env(std::initializer_list<std::pair<double, float>> steps, int sustain, int end)
{
  if(steps.size() > std::size_t(czumarzu::kSteps))
    throw std::invalid_argument("an envelope has eight steps at most");
  EnvSpec e;
  int i = 0;
  for(auto [t, l] : steps)
  {
    e.rate[std::size_t(i)] = rate(t);
    e.level[std::size_t(i)] = l;
    i++;
  }
  for(; i < czumarzu::kSteps; i++)
    e.rate[std::size_t(i)] = 0.5f;
  e.sustain = sustain;
  e.end = end;
  return e;
}

//! A rhythm in the CZ's eight steps: each hit a near-instant rise to its
//! level, then a fall to `floor` over the rest of its period. A step's time
//! is its rate times the distance it moves, so each rate comes from the time
//! wanted. Four hits at most; after the last, the level stays at `floor`,
//! or with `loop`, the pattern starts again while the key is held.
EnvSpec hits(
    std::initializer_list<std::pair<double, float>> pattern, float floor = 0.f,
    bool loop = false)
{
  if(pattern.size() > std::size_t(czumarzu::kSteps / 2))
    throw std::invalid_argument("four hits at most: each takes two steps");
  // A full-scale rise in 0.5 ms: each hit rises from the floor
  constexpr double kRise = 0.0005;
  EnvSpec e;
  int i = 0;
  for(auto [period, level] : pattern)
  {
    const double distance = std::abs(level - floor);
    e.rate[std::size_t(i)] = rate(kRise);
    e.level[std::size_t(i)] = level;
    i++;
    const double fall = std::max(0.001, period - distance * kRise);
    e.rate[std::size_t(i)] = rate(fall / std::max(1e-3, distance));
    e.level[std::size_t(i)] = floor;
    i++;
  }
  for(int k = i; k < czumarzu::kSteps; k++)
    e.rate[std::size_t(k)] = 0.5f;
  e.sustain = floor > 0.f ? i - 1 : -1;
  e.end = i - 1;
  e.loop = loop;
  return e;
}

struct CazLine
{
  Wave w1{Wave::Saw};
  SecondWave w2{SecondWave::Off};
  float formant{1.f}, level{1.f}, dcwDepth{1.f}, dcoDepth{12.f}, dcwKf{0.f}, dcaKf{0.f};
  EnvSpec dca = env({{0.002, 1.f}, {0.3, 0.f}}, 0, 1);
  EnvSpec dcw = env({{0.01, 0.6f}, {1.5, 0.3f}, {0.3, 0.f}}, 1, 2);
  EnvSpec dco = env({{0.001, 0.f}}, -1, 0);
};

void setLine(czumarzu::CZumarzu& c, int n, const CazLine& l)
{
  auto& in = c.inputs;
  // An envelope's four ports
  auto put = [](const EnvSpec& e, auto& steps, auto& sustain, auto& end, auto& loop) {
    steps.value = e.points();
    sustain.value = czumarzu::SustainStep(e.sustainItem());
    end.value = czumarzu::EndStep(e.end);
    loop.value = e.loop;
  };
#define CZUMARZU_SET_LINE(N)                                                            \
  in.l##N##_wave1.value = l.w1;                                                         \
  in.l##N##_wave2.value = l.w2;                                                         \
  in.l##N##_formant.value = l.formant;                                                  \
  in.l##N##_level.value = l.level;                                                      \
  in.l##N##_dcw_depth.value = l.dcwDepth;                                               \
  in.l##N##_dco_depth.value = l.dcoDepth;                                               \
  in.l##N##_dcw_kf.value = l.dcwKf;                                                     \
  in.l##N##_dca_kf.value = l.dcaKf;                                                     \
  put(l.dca, in.l##N##_dca, in.l##N##_dca_sustain, in.l##N##_dca_end, in.l##N##_dca_loop); \
  put(l.dcw, in.l##N##_dcw, in.l##N##_dcw_sustain, in.l##N##_dcw_end, in.l##N##_dcw_loop); \
  put(l.dco, in.l##N##_dco, in.l##N##_dco_sustain, in.l##N##_dco_end, in.l##N##_dco_loop);
  if(n == 1)
  {
    CZUMARZU_SET_LINE(1)
  }
  else
  {
    CZUMARZU_SET_LINE(2)
  }
#undef CZUMARZU_SET_LINE
}

void czumarzuPresets(const std::filesystem::path& root)
{
  const auto dir = presetFolder(root, "CZumarzu");
  struct Preset
  {
    const char* name;
    const char* category;
    std::function<void(czumarzu::CZumarzu&)> set;
  };
  using LM = czumarzu::LineMode;
  using Mod = czumarzu::Modulation;
  const std::vector<Preset> presets = {
      {"Brass Section", "Brass",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.dca = env({{0.04, 1.f}, {2.0, 0.85f}, {0.25, 0.f}}, 1, 2);
         l.dcw = env({{0.12, 0.75f}, {0.8, 0.5f}, {0.3, 0.1f}}, 1, 2);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 7.f;
         c.inputs.vib_depth.value = 0.15f;
         c.inputs.vib_delay.value = 0.4f;
         c.inputs.vel_dcw.value = 0.6f;
       }},
      {"Resonant Bass", "Bass",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::ResoSaw;
         l.dca = env({{0.002, 1.f}, {1.5, 0.7f}, {0.08, 0.f}}, 1, 2);
         l.dcw = env({{0.002, 0.9f}, {0.4, 0.15f}, {0.1, 0.f}}, 1, 2);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1;
         c.inputs.octave.value = -1;
         c.inputs.play_mode.value = czumarzu::PlayMode::Mono;
         c.inputs.vel_dcw.value = 0.7f;
       }},
      {"Digital Piano", "Keys",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dca = env({{0.002, 1.f}, {4.0, 0.f}, {0.2, 0.f}}, -1, 1);
         a.dcw = env({{0.002, 0.45f}, {1.2, 0.05f}}, -1, 1);
         a.dcaKf = 0.6f;
         CazLine b;
         b.w1 = Wave::Square;
         b.level = 0.45f;
         b.dca = env({{0.002, 1.f}, {0.9, 0.f}}, -1, 1);
         b.dcw = env({{0.002, 0.6f}, {0.5, 0.f}}, -1, 1);
         b.dcaKf = 0.8f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.detune_note.value = 0;
         c.inputs.detune_oct.value = 1;
         c.inputs.detune_fine.value = 3.f;
         c.inputs.vel_dcw.value = 0.8f;
       }},
      {"Glass Bells", "Bells",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dcwDepth = 0.f;
         a.dca = env({{0.002, 1.f}, {6.0, 0.f}}, -1, 1);
         CazLine b = a;
         b.dca = env({{0.002, 1.f}, {3.0, 0.f}}, -1, 1);
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Ring;
         c.inputs.detune_oct.value = 1;
         c.inputs.detune_note.value = 7;
         c.inputs.detune_fine.value = 0.f;
         c.inputs.spread.value = 0.f;
       }},
      {"Noise Sweep", "Effects",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoTrapezoid;
         a.level = 0.6f;
         a.dca = env({{1.5, 1.f}, {3.0, 0.f}}, 0, 1);
         a.dcw = env({{3.0, 1.f}, {3.0, 0.2f}, {2.0, 0.f}}, 1, 2);
         CazLine b = a;
         b.w1 = Wave::ResoTriangle;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Noise;
       }},
      {"Sync Lead", "Lead",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoSaw;
         a.dca = env({{0.004, 1.f}, {0.15, 0.f}}, 0, 1);
         a.dcw = env({{0.25, 0.65f}, {0.2, 0.f}}, 0, 1);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 5.f;
         c.inputs.play_mode.value = czumarzu::PlayMode::Legato;
         c.inputs.glide.value = 0.06f;
         c.inputs.vib_depth.value = 0.25f;
         c.inputs.vib_delay.value = 0.3f;
         c.inputs.pressure_dcw.value = 0.4f;
       }},
      {"Soft Pad", "Pad",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dca = env({{1.2, 1.f}, {2.5, 0.f}}, 0, 1);
         a.dcw = env({{2.0, 0.25f}, {2.0, 0.f}}, 0, 1);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 12.f;
         c.inputs.spread.value = 0.7f;
         c.inputs.vel_dcw.value = 0.f;
       }},
      {"Strings", "Pad",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dca = env({{0.5, 1.f}, {1.2, 0.f}}, 0, 1);
         a.dcw = env({{0.6, 0.55f}, {1.0, 0.f}}, 0, 1);
         CazLine b = a;
         b.w1 = Wave::Square;
         b.level = 0.6f;
         b.dcw = env({{0.6, 0.3f}, {1.0, 0.f}}, 0, 1);
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.detune_fine.value = 9.f;
         c.inputs.vib_depth.value = 0.12f;
         c.inputs.vib_delay.value = 0.5f;
         c.inputs.spread.value = 0.6f;
       }},
      {"Clav", "Keys",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Pulse;
         a.dca = env({{0.001, 1.f}, {1.2, 0.f}, {0.05, 0.f}}, -1, 1);
         a.dcw = env({{0.001, 0.85f}, {0.35, 0.3f}}, -1, 1);
         a.dcaKf = 0.5f;
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.vel_dcw.value = 0.8f;
         c.inputs.vel_level.value = 0.7f;
       }},
      {"Organ", "Keys",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::DoubleSine;
         a.dcwDepth = 0.f;
         a.dca = env({{0.003, 1.f}, {0.05, 0.f}}, 0, 1);
         CazLine b;
         b.w1 = Wave::Square;
         b.level = 0.5f;
         b.dca = a.dca;
         b.dcw = env({{0.003, 0.15f}, {0.05, 0.f}}, 0, 1);
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.detune_oct.value = -1;
         c.inputs.detune_fine.value = 0.f;
         c.inputs.vib_wave.value = czumarzu::VibratoWave::Sine;
         c.inputs.vib_depth.value = 0.08f;
         c.inputs.vel_level.value = 0.f;
         c.inputs.vel_dcw.value = 0.f;
       }},
      {"Wah Pluck", "Pluck",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoTriangle;
         a.dca = env({{0.001, 1.f}, {0.8, 0.f}}, -1, 1);
         a.dcw = env({{0.001, 1.f}, {0.25, 0.f}}, -1, 1);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.vel_dcw.value = 0.7f;
       }},
      {"Chip Arp", "Chip",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Pulse;
         a.dca = env({{0.001, 1.f}, {0.3, 0.6f}, {0.02, 0.f}}, 1, 2);
         a.dcw = env({{0.001, 0.92f}}, 0, 0);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.vel_dcw.value = 0.f;
         c.inputs.vel_level.value = 0.f;
         c.inputs.spread.value = 0.f;
       }},
      {"Formant Choir", "Pad",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Vps;
         a.formant = 3.f;
         a.dca = env({{0.8, 1.f}, {1.5, 0.f}}, 0, 1);
         a.dcw = env({{1.0, 0.45f}, {1.0, 0.f}}, 0, 1);
         CazLine b = a;
         b.formant = 4.5f;
         b.level = 0.7f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.detune_fine.value = 6.f;
         c.inputs.vib_depth.value = 0.12f;
         c.inputs.vib_delay.value = 0.6f;
         c.inputs.spread.value = 0.6f;
       }},
      {"Metal Pad", "Pad",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dca = env({{1.5, 1.f}, {3.0, 0.f}}, 0, 1);
         a.dcw = env({{2.0, 0.2f}, {2.0, 0.f}}, 0, 1);
         CazLine b = a;
         b.w1 = Wave::ResoPulse;
         b.dcw = env({{4.0, 0.7f}, {4.0, 0.2f}, {2.0, 0.f}}, 1, 2);
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Ring;
         c.inputs.detune_note.value = 5;
         c.inputs.spread.value = 0.5f;
       }},
      {"Saw-Pulse Lead", "Lead",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::SawPulse;
         a.dca = env({{0.005, 1.f}, {0.2, 0.f}}, 0, 1);
         a.dcw = env({{0.08, 0.7f}, {1.0, 0.45f}, {0.2, 0.f}}, 1, 2);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = -6.f;
         c.inputs.play_mode.value = czumarzu::PlayMode::Mono;
         c.inputs.glide.value = 0.04f;
         c.inputs.vib_depth.value = 0.2f;
         c.inputs.vib_delay.value = 0.35f;
       }},
      {"Bitter Keys", "Keys",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::DoublePulse;
         a.dca = env({{0.002, 1.f}, {2.0, 0.f}}, -1, 1);
         a.dcw = env({{0.002, 0.7f}, {0.6, 0.1f}}, -1, 1);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.quality.value = czumarzu::Quality::Clean2x;
       }},
      {"Hollow Flute", "Lead",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Square;
         a.dca = env({{0.08, 1.f}, {0.15, 0.f}}, 0, 1);
         a.dcw = env({{0.1, 0.12f}, {0.2, 0.f}}, 0, 1);
         CazLine b = a;
         b.w1 = Wave::ResoTrapezoid;
         b.level = 0.12f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Noise;
         c.inputs.vib_depth.value = 0.15f;
         c.inputs.vib_delay.value = 0.5f;
         c.inputs.play_mode.value = czumarzu::PlayMode::Legato;
         c.inputs.glide.value = 0.03f;
       }},
      {"Tine Piano", "Keys",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dcwDepth = 0.f;
         a.dca = env({{0.002, 1.f}, {5.0, 0.f}}, -1, 1);
         a.dcaKf = 0.5f;
         CazLine b;
         b.w1 = Wave::ResoSaw;
         b.level = 0.35f;
         b.dca = env({{0.001, 1.f}, {0.25, 0.f}}, -1, 1);
         b.dcw = env({{0.001, 0.5f}, {0.2, 0.1f}}, -1, 1);
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.detune_oct.value = 2;
         c.inputs.detune_fine.value = 0.f;
         c.inputs.vel_level.value = 0.7f;
         c.inputs.vel_dcw.value = 0.9f;
       }},
      {"Detuned Saws", "Lead",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dca = env({{0.005, 1.f}, {0.3, 0.f}}, 0, 1);
         a.dcw = env({{0.005, 0.95f}, {0.3, 0.f}}, 0, 1);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 16.f;
         c.inputs.spread.value = 0.9f;
         c.inputs.vel_dcw.value = 0.2f;
       }},
      {"Acid Line", "Bass",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoSaw;
         a.dca = env({{0.002, 1.f}, {0.6, 0.8f}, {0.05, 0.f}}, 1, 2);
         a.dcw = env({{0.002, 0.95f}, {0.18, 0.2f}, {0.05, 0.f}}, 1, 2);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.octave.value = -1;
         c.inputs.play_mode.value = czumarzu::PlayMode::Legato;
         c.inputs.glide.value = 0.06f;
         c.inputs.vel_dcw.value = 0.9f;
       }},
      {"Slow Morph", "Pad",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w2 = SecondWave::Square;
         a.dca = env({{1.0, 1.f}, {3.0, 0.f}}, 0, 1);
         // All eight steps: a rise, a dip, a second rise, then down to rest
         a.dcw = env(
             {{1.5, 0.6f}, {1.0, 0.2f}, {1.5, 0.8f}, {2.0, 0.4f}, {1.0, 0.55f}, {1.0, 0.35f},
              {2.0, 0.45f}, {3.0, 0.f}},
             6, 7);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 8.f;
         c.inputs.spread.value = 0.6f;
         c.inputs.vel_dcw.value = 0.f;
       }},
      {"Kalimba", "Pluck",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::DoubleSine;
         a.dca = env({{0.001, 1.f}, {1.5, 0.f}}, -1, 1);
         a.dcw = env({{0.001, 0.5f}, {0.08, 0.f}}, -1, 1);
         a.dcaKf = 0.6f;
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.octave.value = 1;
         c.inputs.vel_dcw.value = 0.6f;
       }},
      {"Pressure Lead", "Lead",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dca = env({{0.01, 1.f}, {0.2, 0.f}}, 0, 1);
         a.dcw = env({{0.01, 0.2f}, {0.2, 0.f}}, 0, 1);
         CazLine b = a;
         b.w1 = Wave::ResoTriangle;
         b.level = 0.5f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.pressure_dcw.value = 0.8f;
         c.inputs.vib_depth.value = 0.2f;
         c.inputs.vib_delay.value = 0.4f;
         c.inputs.play_mode.value = czumarzu::PlayMode::Legato;
         c.inputs.glide.value = 0.05f;
       }},
      {"Pitch Drop Tom", "Percussion",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dcwDepth = 0.3f;
         a.dca = env({{0.001, 1.f}, {0.6, 0.f}}, -1, 1);
         a.dcw = env({{0.001, 1.f}, {0.1, 0.f}}, -1, 1);
         a.dco = env({{0.001, 1.f}, {0.25, 0.f}}, -1, 1);
         a.dcoDepth = 12.f;
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.octave.value = -1;
       }},
      // ---- rhythmic: hits on a grid, at 120 bpm
      {"Sixteenths", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::Square;
         l.w2 = SecondWave::Saw;
         l.dca = hits({{0.125, 1.f}, {0.125, 0.9f}, {0.125, 0.95f}, {0.125, 0.9f}}, 0.f, true);
         // The first of four brighter
         l.dcw = hits(
             {{0.125, 0.85f}, {0.125, 0.45f}, {0.125, 0.6f}, {0.125, 0.45f}}, 0.1f, true);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 5.f;
         c.inputs.vel_dcw.value = 0.4f;
       }},
      {"Triplet Echo", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::ResoTrapezoid;
         // Eighth-note triplets, each quieter and darker: an echo
         l.dca = hits({{0.1667, 1.f}, {0.1667, 0.86f}, {0.1667, 0.74f}, {0.1667, 0.62f}});
         l.dcw = hits({{0.1667, 0.9f}, {0.1667, 0.65f}, {0.1667, 0.45f}, {0.1667, 0.3f}});
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 6.f;
       }},
      {"Dotted Delay", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Pulse;
         a.dca = hits({{0.1875, 1.f}, {0.1875, 0.85f}, {0.1875, 0.72f}, {0.1875, 0.6f}});
         a.dcw = hits({{0.1875, 0.7f}, {0.1875, 0.5f}, {0.1875, 0.35f}, {0.1875, 0.2f}});
         CazLine b = a;
         b.w1 = Wave::DoubleSine;
         b.level = 0.5f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.detune_oct.value = 1;
         c.inputs.detune_fine.value = 0.f;
       }},
      {"Stutter", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::SawPulse;
         // Thirty-second notes, then the note holds
         l.dca = hits({{0.0625, 1.f}, {0.0625, 1.f}, {0.0625, 1.f}, {0.0625, 1.f}}, 0.82f);
         l.dcw = EnvSpec::make({{rate(0.002), 0.8f}, {rate(0.5), 0.4f}, {rate(0.3), 0.f}}, 1, 2);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 7.f;
       }},
      {"Filter Ticks", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::ResoSaw;
         // The level holds; the resonance ticks on sixteenths
         l.dca = env({{0.01, 1.f}, {0.3, 0.f}}, 0, 1);
         l.dcw = hits({{0.125, 1.f}, {0.125, 0.8f}, {0.125, 0.9f}, {0.125, 0.7f}}, 0.15f, true);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 6.f;
         c.inputs.vel_dcw.value = 0.f;
       }},
      {"Laser Hits", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::ResoSaw;
         l.dca = hits({{0.125, 1.f}, {0.125, 0.9f}, {0.125, 1.f}, {0.125, 0.9f}}, 0.f, true);
         l.dcw = hits({{0.125, 1.f}, {0.125, 1.f}, {0.125, 1.f}, {0.125, 1.f}}, 0.2f, true);
         // Each hit a fall of an octave
         l.dco = hits({{0.125, 1.f}, {0.125, 1.f}, {0.125, 1.f}, {0.125, 1.f}}, 0.f, true);
         l.dcoDepth = 12.f;
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 9.f;
       }},
      {"Gallop", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::Saw;
         l.w2 = SecondWave::Pulse;
         // Long, short, short, over and over
         l.dca = hits({{0.25, 1.f}, {0.125, 0.8f}, {0.125, 0.85f}}, 0.f, true);
         l.dcw = hits({{0.25, 0.8f}, {0.125, 0.5f}, {0.125, 0.55f}}, 0.1f, true);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 4.f;
       }},
      {"Clave", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::DoubleSine;
         l.dcwDepth = 0.5f;
         // Three, three, two, over and over: the tresillo
         l.dca = hits({{0.375, 1.f}, {0.375, 0.95f}, {0.25, 1.f}}, 0.f, true);
         l.dcw = hits({{0.375, 0.6f}, {0.375, 0.6f}, {0.25, 0.6f}}, 0.f, true);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1;
         c.inputs.octave.value = 1;
       }},
      {"Octave Trill", "Rhythmic",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::Square;
         l.dca = env({{0.005, 1.f}, {0.2, 0.f}}, 0, 1);
         l.dcw = env({{0.01, 0.5f}, {0.2, 0.f}}, 0, 1);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 5.f;
         // A square vibrato of an octave either way: sixteenths at 120 bpm
         c.inputs.vib_wave.value = czumarzu::VibratoWave::Square;
         c.inputs.vib_period.value = 0.25f;
         c.inputs.vib_depth.value = 12.f;
       }},
      // ---- the second batch: alternation, modulation, formants
      {"Talking Wire", "Creative",
       [](czumarzu::CZumarzu& c) {
         CazLine l;
         l.w1 = Wave::Vps;
         l.formant = 5.f;
         l.dca = env({{0.01, 1.f}, {0.4, 0.f}}, 0, 1);
         // A vowel-like walk of the pivot
         l.dcw = env({{0.08, 0.2f}, {0.12, 0.7f}, {0.1, 0.35f}, {0.15, 0.85f}, {0.12, 0.25f},
                      {0.2, 0.6f}, {0.3, 0.45f}, {0.3, 0.f}},
                     6, 7);
         setLine(c, 1, l);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 4.f;
         c.inputs.vib_depth.value = 0.12f;
         c.inputs.vib_delay.value = 0.3f;
       }},
      {"Ring Swarm", "Textures",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoTriangle;
         a.w2 = SecondWave::Square;
         a.dca = env({{1.5, 1.f}, {3.0, 0.f}}, 0, 1);
         a.dcw = env({{4.0, 0.7f}, {4.0, 0.3f}, {3.0, 0.f}}, 1, 2);
         CazLine b = a;
         b.w1 = Wave::Saw;
         b.w2 = SecondWave::Off;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Ring;
         c.inputs.detune_note.value = 7;
         c.inputs.detune_fine.value = 13.f;
         c.inputs.spread.value = 0.8f;
       }},
      {"Broken Radio", "Effects",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoPulse;
         a.dca = env({{0.05, 1.f}, {1.0, 0.f}}, 0, 1);
         a.dcw = env({{0.04, 0.9f}, {0.06, 0.3f}, {0.05, 0.8f}, {0.08, 0.2f}, {0.06, 0.7f},
                      {0.1, 0.4f}, {0.2, 0.6f}, {0.5, 0.f}},
                     6, 7);
         a.dco = env({{0.05, 0.3f}, {0.08, 0.f}, {0.05, 0.2f}, {0.2, 0.f}}, -1, 3);
         a.dcoDepth = 7.f;
         CazLine b = a;
         b.w1 = Wave::Square;
         b.level = 0.4f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Noise;
       }},
      {"Glass Harmonica", "Bells",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::DoubleSine;
         a.dcwDepth = 0.25f;
         a.dca = env({{0.15, 1.f}, {6.0, 0.f}, {1.0, 0.f}}, -1, 1);
         a.dcw = env({{0.3, 0.4f}, {3.0, 0.f}}, -1, 1);
         CazLine b = a;
         b.w1 = Wave::Saw;
         b.dcwDepth = 0.f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Ring;
         c.inputs.detune_oct.value = 2;
         c.inputs.detune_fine.value = 3.f;
         c.inputs.vib_wave.value = czumarzu::VibratoWave::Sine;
         c.inputs.vib_depth.value = 0.06f;
       }},
      {"Alternating Organ", "Keys",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         // Saw and square on alternate cycles: a sub octave in the timbre
         a.w1 = Wave::Saw;
         a.w2 = SecondWave::Square;
         a.dca = env({{0.004, 1.f}, {0.06, 0.f}}, 0, 1);
         a.dcw = env({{0.004, 0.3f}, {0.06, 0.f}}, 0, 1);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 2.f;
         c.inputs.vel_level.value = 0.f;
         c.inputs.vel_dcw.value = 0.f;
         c.inputs.vib_wave.value = czumarzu::VibratoWave::Sine;
         c.inputs.vib_depth.value = 0.1f;
       }},
      {"Sub Pulse Lead", "Lead",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Pulse;
         a.w2 = SecondWave::Saw;
         a.dca = env({{0.005, 1.f}, {0.15, 0.f}}, 0, 1);
         a.dcw = env({{0.01, 0.75f}, {0.5, 0.5f}, {0.2, 0.f}}, 1, 2);
         // A fall into the note
         a.dco = env({{0.001, 1.f}, {0.08, 0.f}}, -1, 1);
         a.dcoDepth = 5.f;
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.play_mode.value = czumarzu::PlayMode::Legato;
         c.inputs.glide.value = 0.07f;
         c.inputs.vib_depth.value = 0.2f;
         c.inputs.vib_delay.value = 0.4f;
       }},
      {"Formant Pad", "Pad",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Vps;
         a.formant = 2.5f;
         a.dca = env({{1.2, 1.f}, {2.0, 0.f}}, 0, 1);
         a.dcw = env({{2.5, 0.55f}, {2.0, 0.f}}, 0, 1);
         CazLine b = a;
         b.formant = 6.f;
         b.level = 0.6f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.detune_fine.value = -9.f;
         c.inputs.vib_depth.value = 0.15f;
         c.inputs.vib_delay.value = 0.8f;
         c.inputs.pressure_dcw.value = 0.5f;
         c.inputs.spread.value = 0.7f;
       }},
      {"Zapper", "Effects",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoSaw;
         a.dca = env({{0.001, 1.f}, {0.35, 0.f}}, -1, 1);
         a.dcw = env({{0.001, 1.f}, {0.3, 0.f}}, -1, 1);
         a.dco = env({{0.001, 1.f}, {0.18, 0.f}}, -1, 1);
         a.dcoDepth = 36.f;
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.octave.value = -1;
       }},
      {"Metallic Rain", "Pluck",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoDoubleSaw;
         a.dca = env({{0.001, 1.f}, {0.6, 0.f}}, -1, 1);
         a.dcw = env({{0.001, 0.8f}, {0.3, 0.1f}}, -1, 1);
         a.dcaKf = 0.8f;
         a.dcwKf = 0.5f;
         CazLine b = a;
         b.w1 = Wave::Square;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Ring;
         c.inputs.detune_oct.value = 1;
         c.inputs.detune_note.value = 3;
         c.inputs.octave.value = 1;
         c.inputs.spread.value = 0.9f;
       }},
      {"Hollow Choir", "Pad",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::ResoTrapezoid;
         a.w2 = SecondWave::DoubleSine;
         a.dca = env({{1.0, 1.f}, {2.5, 0.f}}, 0, 1);
         a.dcw = env({{2.0, 0.45f}, {3.0, 0.3f}, {2.0, 0.f}}, 1, 2);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 11.f;
         c.inputs.vib_depth.value = 0.12f;
         c.inputs.vib_delay.value = 0.7f;
         c.inputs.spread.value = 0.8f;
       }},
      {"Deep Space", "Textures",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.dca = env({{3.0, 1.f}, {6.0, 0.f}}, 0, 1);
         // A slow climb of brightness, eight steps over twenty seconds
         a.dcw = env({{3.0, 0.1f}, {3.0, 0.3f}, {2.0, 0.2f}, {3.0, 0.5f}, {2.0, 0.35f},
                      {3.0, 0.7f}, {4.0, 0.6f}, {6.0, 0.f}},
                     6, 7);
         CazLine b = a;
         b.w1 = Wave::ResoTrapezoid;
         b.level = 0.25f;
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Noise;
         c.inputs.detune_oct.value = -1;
         c.inputs.detune_fine.value = 25.f;
         c.inputs.spread.value = 1.f;
       }},
      {"Grit Bass", "Bass",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::DoublePulse;
         a.w2 = SecondWave::Saw;
         a.dca = env({{0.002, 1.f}, {0.8, 0.7f}, {0.06, 0.f}}, 1, 2);
         a.dcw = env({{0.002, 0.8f}, {0.25, 0.3f}, {0.06, 0.f}}, 1, 2);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1;
         c.inputs.octave.value = -1;
         c.inputs.play_mode.value = czumarzu::PlayMode::Mono;
         c.inputs.quality.value = czumarzu::Quality::Clean2x;
       }},
      {"Pressure Brass", "Brass",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Vps;
         a.formant = 1.5f;
         a.dca = env({{0.06, 1.f}, {0.25, 0.f}}, 0, 1);
         a.dcw = env({{0.15, 0.4f}, {0.25, 0.f}}, 0, 1);
         setLine(c, 1, a);
         c.inputs.mode.value = LM::Line1Doubled;
         c.inputs.detune_fine.value = 6.f;
         c.inputs.pressure_dcw.value = 1.f;
         c.inputs.vel_dcw.value = 0.6f;
         c.inputs.vib_depth.value = 0.15f;
         c.inputs.vib_delay.value = 0.5f;
       }},
      {"Toy Piano", "Keys",
       [](czumarzu::CZumarzu& c) {
         CazLine a;
         a.w1 = Wave::Pulse;
         a.dca = env({{0.001, 1.f}, {1.2, 0.f}}, -1, 1);
         a.dcw = env({{0.001, 0.6f}, {0.2, 0.2f}}, -1, 1);
         a.dcaKf = 0.7f;
         CazLine b;
         b.w1 = Wave::DoubleSine;
         b.level = 0.5f;
         b.dca = env({{0.001, 1.f}, {0.3, 0.f}}, -1, 1);
         b.dcw = env({{0.001, 0.3f}, {0.1, 0.f}}, -1, 1);
         setLine(c, 1, a);
         setLine(c, 2, b);
         c.inputs.mode.value = LM::Line1And2;
         c.inputs.modulation.value = Mod::Ring;
         c.inputs.detune_oct.value = 1;
         c.inputs.detune_note.value = 7;
         c.inputs.detune_fine.value = 0.f;
         c.inputs.octave.value = 1;
       }},
  };

  for(const auto& p : presets)
  {
    auto c = std::make_unique<czumarzu::CZumarzu>();
    // Both lines stored in full, even where the preset plays one
    setLine(*c, 1, CazLine{});
    setLine(*c, 2, CazLine{});
    p.set(*c);
    write(dir, p.name, p.category, *c,
          c->inputs.level);
  }
}

// ---- ViceVersa -----------------------------------------------------------------
using viceversa::BankId;

//! One of the four LFOs: what it moves, its wave, period (s), depth, phase
//! (degrees) and delay (s)
void lfo(viceversa::ViceVersa& r, int n, viceversa::ModTarget target, viceversa::LfoShape shape,
         float period, float depth, float phase = 0.f, float delay = 0.f)
{
  auto& in = r.inputs;
  auto set = [&](auto& t, auto& sh, auto& pe, auto& de, auto& ph, auto& dl) {
    t.value = target;
    sh.value = shape;
    pe.value = period;
    de.value = depth;
    ph.value = phase;
    dl.value = delay;
  };
  switch(n)
  {
#define VICEVERSA_SET_LFO(N)                                                            \
  case N:                                                                              \
    set(in.lfo##N##_target, in.lfo##N##_shape, in.lfo##N##_period, in.lfo##N##_depth,  \
        in.lfo##N##_phase, in.lfo##N##_delay);                                         \
    break;
    VICEVERSA_SET_LFO(1)
    VICEVERSA_SET_LFO(2)
    VICEVERSA_SET_LFO(3)
    VICEVERSA_SET_LFO(4)
#undef VICEVERSA_SET_LFO
  }
}

//! A vibrato of `semitones` on LFO 3: the pitch depth is on a squared curve
void vibrato(viceversa::ViceVersa& r, float semitones, float period, float delay)
{
  lfo(r, 3, viceversa::ModTarget::Pitch, viceversa::LfoShape::Triangle, period,
      std::sqrt(semitones / viceversa::kModPitch), 0.f, delay);
}

void vec(viceversa::ViceVersa& r, std::array<float, 5> x, std::array<float, 5> y, std::array<float, 4> t,
         float release, viceversa::VecLoop loop = viceversa::VecLoop::Off, int from = 1, int to = 3)
{
  viceversa::VecEnvSpec s;
  s.x = x;
  s.y = y;
  auto& in = r.inputs;
  in.vec_env.value = s.points();
  in.vec_loop.value = loop;
  in.loop_from.value = viceversa::LoopFrom(from);
  in.loop_to.value = viceversa::loopToItem(to);
  in.move1.value = t[0];
  in.move2.value = t[1];
  in.move3.value = t[2];
  in.move4.value = t[3];
  in.move_release.value = release;
}

void sources(viceversa::ViceVersa& r, std::array<BankId, 4> b, std::array<float, 4> frame = {0, 0, 0, 0},
             std::array<int, 4> coarse = {0, 0, 0, 0}, std::array<float, 4> fine = {0, 0, 0, 0})
{
  auto& in = r.inputs;
  in.srcA_bank.value = b[0];
  in.srcB_bank.value = b[1];
  in.srcC_bank.value = b[2];
  in.srcD_bank.value = b[3];
  in.srcA_frame.value = frame[0];
  in.srcB_frame.value = frame[1];
  in.srcC_frame.value = frame[2];
  in.srcD_frame.value = frame[3];
  in.srcA_coarse.value = coarse[0];
  in.srcB_coarse.value = coarse[1];
  in.srcC_coarse.value = coarse[2];
  in.srcD_coarse.value = coarse[3];
  in.srcA_fine.value = fine[0];
  in.srcB_fine.value = fine[1];
  in.srcC_fine.value = fine[2];
  in.srcD_fine.value = fine[3];
}

void adsr(viceversa::ViceVersa& r, float a, float d, float s, float rel, float fa, float fd, float fs,
          float fr)
{
  auto& in = r.inputs;
  in.a_attack.value = a;
  in.a_decay.value = d;
  in.a_sustain.value = s;
  in.a_release.value = rel;
  in.f_attack.value = fa;
  in.f_decay.value = fd;
  in.f_sustain.value = fs;
  in.f_release.value = fr;
}

void filter(viceversa::ViceVersa& r, float cutoff, float reso, float envAmt, float drive = 0.2f,
            float key = 0.5f)
{
  auto& in = r.inputs;
  in.cutoff.value = cutoff;
  in.resonance.value = reso;
  in.filter_env.value = envAmt;
  in.drive.value = drive;
  in.key_track.value = key;
}

void viceVersaPresets(const std::filesystem::path& root)
{
  const auto dir = presetFolder(root, "ViceVersa");
  using L = viceversa::VecLoop;
  struct Preset
  {
    const char* name;
    const char* category;
    std::function<void(viceversa::ViceVersa&)> set;
  };
  const std::vector<Preset> presets = {
      {"Vector Pad", "Pad",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SineSaw, BankId::Vowels, BankId::Glass, BankId::Organ},
                 {0.6f, 0.3f, 0.5f, 0.4f});
         vec(r, {-0.7f, 0.f, 0.7f, 0.f, 0.f}, {0.f, 0.7f, 0.f, -0.7f, 0.f},
                  {1.5f, 2.0f, 2.0f, 2.0f}, 1.0f, L::Round, 1, 3);
         adsr(r, 0.8f, 1.f, 1.f, 1.8f, 1.f, 2.f, 0.6f, 1.5f);
         filter(r, 2500.f, 0.15f, 18.f);
         r.inputs.spread.value = 0.7f;
       }},
      {"Vowel Morph", "Pad",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Vowels, BankId::Vowels, BankId::Vowels, BankId::Vowels},
                 {0.f, 0.5f, 1.f, 0.25f}, {0, 0, 0, 0}, {-4.f, 3.f, 5.f, -2.f});
         vec(r, {-1.f, 0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, -1.f, 0.f},
                  {0.8f, 0.8f, 0.8f, 0.8f}, 0.6f, L::BackAndForth, 0, 3);
         adsr(r, 0.3f, 1.f, 1.f, 1.f, 0.2f, 1.f, 0.8f, 1.f);
         filter(r, 6000.f, 0.f, 0.f);
         vibrato(r, 0.1f, 0.2f, 0.6f);
       }},
      {"Digital Bells", "Bells",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Glass, BankId::Fm, BankId::Glass, BankId::Fm}, {0.9f, 0.7f, 0.4f, 0.3f},
                 {0, 12, 19, 0});
         vec(r, {0.f, -0.5f, -1.f, -0.6f, -0.8f}, {1.f, 0.4f, 0.f, -0.2f, 0.f},
                  {0.05f, 0.3f, 0.8f, 2.0f}, 1.5f);
         adsr(r, 0.002f, 3.f, 0.f, 2.f, 0.002f, 1.f, 0.f, 1.f);
         filter(r, 9000.f, 0.f, 12.f);
         r.inputs.frame_env.value = 0.3f;
       }},
      {"Sync Lead", "Lead",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Sync, BankId::Sync, BankId::SineSaw, BankId::Sync}, {0.f, 0.3f, 1.f, 0.6f},
                 {0, 0, -12, 0}, {0.f, 6.f, 0.f, -6.f});
         r.inputs.position.value.x = -0.4f;
         r.inputs.vec_depth.value = 0.f;
         r.inputs.frame_env.value = 0.7f;
         adsr(r, 0.005f, 0.4f, 0.9f, 0.2f, 0.005f, 0.6f, 0.2f, 0.3f);
         filter(r, 4000.f, 0.25f, 24.f, 0.4f);
         r.inputs.play_mode.value = viceversa::PlayMode::Legato;
         r.inputs.glide.value = 0.05f;
         vibrato(r, 0.2f, 0.18f, 0.35f);
       }},
      {"Fold Bass", "Bass",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Fold, BankId::SquareSaw, BankId::Fold, BankId::SineSaw},
                 {0.3f, 0.f, 0.8f, 0.f}, {0, 0, 0, -12});
         r.inputs.position.value.x = -0.3f;
         r.inputs.position.value.y = -0.2f;
         r.inputs.vec_depth.value = 0.f;
         r.inputs.frame_env.value = 0.5f;
         adsr(r, 0.002f, 0.6f, 0.7f, 0.15f, 0.002f, 0.3f, 0.1f, 0.2f);
         filter(r, 700.f, 0.35f, 30.f, 0.5f);
         r.inputs.octave.value = -1;
         r.inputs.play_mode.value = viceversa::PlayMode::Mono;
       }},
      {"Organ Drift", "Keys",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Organ, BankId::Organ, BankId::Organ, BankId::Organ},
                 {0.f, 0.4f, 0.8f, 1.f});
         r.inputs.vec_depth.value = 0.f;
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Sine, 6.f, 0.8f);
         lfo(r, 2, viceversa::ModTarget::Y, viceversa::LfoShape::Sine, 6.f, 0.3f, 90.f);
         adsr(r, 0.003f, 0.1f, 1.f, 0.08f, 0.003f, 0.1f, 1.f, 0.1f);
         filter(r, 12000.f, 0.f, 0.f);
         r.inputs.vel_level.value = 0.f;
       }},
      {"Harmonic Choir", "Pad",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::HarmonicSweep, BankId::Vowels, BankId::HarmonicSweep, BankId::Vowels},
                 {0.1f, 0.2f, 0.35f, 0.8f}, {0, 0, 12, 0}, {-5.f, 4.f, 2.f, -3.f});
         vec(r, {-0.5f, 0.f, 0.5f, 0.f, 0.f}, {0.f, 0.5f, 0.f, -0.5f, 0.f},
                  {3.0f, 3.0f, 3.0f, 3.0f}, 2.0f, L::Round, 0, 3);
         adsr(r, 1.2f, 1.f, 1.f, 2.5f, 1.f, 1.f, 1.f, 2.f);
         filter(r, 5000.f, 0.f, 0.f);
         r.inputs.spread.value = 0.8f;
       }},
      {"PWM Strings", "Pad",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::PulseWidth, BankId::PulseWidth, BankId::PulseWidth, BankId::PulseWidth},
                 {0.f, 0.4f, 0.8f, 0.6f}, {0, 0, 0, 0}, {-7.f, 0.f, 7.f, 3.f});
         vec(r, {-1.f, 0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, -1.f, 0.f},
                  {0.6f, 0.6f, 0.6f, 0.6f}, 1.f, L::Round, 0, 3);
         adsr(r, 0.4f, 1.f, 1.f, 0.9f, 0.5f, 1.f, 0.7f, 1.f);
         filter(r, 3500.f, 0.1f, 12.f);
         r.inputs.spread.value = 0.8f;
       }},
      {"Comb Sweep", "Effects",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Comb, BankId::Comb, BankId::Comb, BankId::Comb}, {0.f, 0.3f, 0.6f, 1.f});
         vec(r, {-1.f, 0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, -1.f, 0.f},
                  {2.0f, 2.0f, 2.0f, 2.0f}, 2.0f, L::BackAndForth, 0, 3);
         adsr(r, 0.5f, 1.f, 1.f, 2.f, 0.5f, 1.f, 1.f, 2.f);
         filter(r, 8000.f, 0.3f, 0.f);
       }},
      {"Digital Grit", "Lead",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Digital, BankId::SquareSaw, BankId::Digital, BankId::Fold},
                 {0.5f, 0.5f, 1.f, 0.4f});
         r.inputs.position.value.x = 0.2f;
         r.inputs.vec_depth.value = 0.f;
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Triangle, 0.5f, 0.4f);
         adsr(r, 0.003f, 0.3f, 0.8f, 0.15f, 0.003f, 0.3f, 0.4f, 0.2f);
         filter(r, 3000.f, 0.4f, 24.f, 0.8f);
         r.inputs.play_mode.value = viceversa::PlayMode::Mono;
         r.inputs.glide.value = 0.03f;
       }},
      {"PD Brass", "Brass",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::PhaseDist, BankId::PhaseDist, BankId::SineSaw, BankId::PhaseDist},
                 {0.2f, 0.4f, 0.8f, 0.1f}, {0, 0, 0, 0}, {0.f, 7.f, -5.f, 0.f});
         r.inputs.position.value.y = 0.2f;
         r.inputs.vec_depth.value = 0.f;
         r.inputs.frame_env.value = 0.6f;
         adsr(r, 0.05f, 0.5f, 0.85f, 0.25f, 0.12f, 0.8f, 0.5f, 0.3f);
         filter(r, 2000.f, 0.1f, 24.f, 0.3f);
         vibrato(r, 0.15f, 0.18f, 0.4f);
       }},
      {"Square Saw Bass", "Bass",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SquareSaw, BankId::SquareSaw, BankId::SineSaw, BankId::SquareSaw},
                 {0.f, 1.f, 0.f, 0.5f}, {0, 0, -12, 0});
         r.inputs.position.value.x = -0.5f;
         r.inputs.position.value.y = 0.3f;
         r.inputs.vec_depth.value = 0.f;
         adsr(r, 0.002f, 0.4f, 0.8f, 0.1f, 0.002f, 0.25f, 0.2f, 0.1f);
         filter(r, 500.f, 0.3f, 36.f, 0.4f);
         r.inputs.octave.value = -1;
         r.inputs.play_mode.value = viceversa::PlayMode::Legato;
         r.inputs.glide.value = 0.04f;
       }},
      {"Glass Pluck", "Pluck",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Glass, BankId::Fm, BankId::Glass, BankId::SineSaw},
                 {1.f, 0.5f, 0.3f, 0.2f}, {0, 12, 0, 0});
         vec(r, {0.f, -0.6f, -0.9f, -0.9f, -0.9f}, {1.f, 0.f, 0.f, 0.f, 0.f},
                  {0.03f, 0.2f, 1.0f, 1.0f}, 0.5f);
         adsr(r, 0.001f, 1.2f, 0.f, 0.6f, 0.001f, 0.4f, 0.f, 0.4f);
         filter(r, 5000.f, 0.2f, 24.f);
       }},
      {"Wide Saws", "Lead",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SineSaw, BankId::SineSaw, BankId::SineSaw, BankId::SineSaw},
                 {1.f, 1.f, 1.f, 1.f}, {0, 0, 0, -12}, {-11.f, 7.f, 12.f, 0.f});
         r.inputs.vec_depth.value = 0.f;
         r.inputs.law.value = viceversa::MixLaw::EqualPower;
         adsr(r, 0.01f, 0.5f, 1.f, 0.3f, 0.01f, 0.6f, 0.5f, 0.3f);
         filter(r, 6000.f, 0.1f, 12.f, 0.3f);
         r.inputs.spread.value = 1.f;
       }},
      {"Evolving Texture", "Textures",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Fold, BankId::HarmonicSweep, BankId::Comb, BankId::Vowels},
                 {0.6f, 0.5f, 0.2f, 0.7f}, {0, 12, 0, -12});
         vec(r, {-0.9f, 0.2f, 0.8f, -0.1f, 0.f}, {0.1f, 0.8f, -0.2f, -0.9f, 0.f},
                  {4.0f, 6.0f, 5.0f, 7.0f}, 3.0f, L::BackAndForth, 0, 3);
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Random, 1.5f, 0.15f);
         lfo(r, 2, viceversa::ModTarget::Y, viceversa::LfoShape::Random, 1.5f, 0.15f, 90.f);
         adsr(r, 2.f, 1.f, 1.f, 4.f, 2.f, 1.f, 1.f, 4.f);
         filter(r, 3000.f, 0.25f, 12.f);
         r.inputs.spread.value = 1.f;
       }},
      {"Breathing Pad", "Pad",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SineSaw, BankId::Vowels, BankId::SineSaw, BankId::Glass},
                 {0.2f, 0.6f, 0.7f, 0.2f}, {0, 0, 0, 12});
         r.inputs.position.value.y = -0.6f;
         r.inputs.vec_depth.value = 0.f;
         r.inputs.pressure_y.value = 1.f;
         r.inputs.timbre_x.value = 0.8f;
         adsr(r, 0.6f, 1.f, 1.f, 1.5f, 0.6f, 1.f, 1.f, 1.5f);
         filter(r, 4000.f, 0.f, 0.f);
       }},
      {"Formant Bass", "Bass",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Vowels, BankId::Vowels, BankId::SineSaw, BankId::Vowels},
                 {0.f, 1.f, 0.f, 0.5f}, {0, 0, -12, 0});
         vec(r, {-1.f, 0.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 1.f, 1.f, 1.f},
                  {0.25f, 1.0f, 1.0f, 1.0f}, 0.2f);
         adsr(r, 0.003f, 0.5f, 0.8f, 0.12f, 0.003f, 0.3f, 0.3f, 0.2f);
         filter(r, 900.f, 0.4f, 24.f, 0.4f);
         r.inputs.octave.value = -1;
         r.inputs.play_mode.value = viceversa::PlayMode::Mono;
       }},
      {"Sub Pulse", "Bass",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::PulseWidth, BankId::SineSaw, BankId::PulseWidth, BankId::SineSaw},
                 {0.6f, 0.f, 0.9f, 0.f}, {0, -12, 0, -12});
         r.inputs.vec_depth.value = 0.f;
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Sine, 4.f, 0.6f);
         adsr(r, 0.003f, 0.3f, 0.9f, 0.1f, 0.003f, 0.3f, 0.5f, 0.1f);
         filter(r, 1200.f, 0.2f, 12.f, 0.3f);
         r.inputs.octave.value = -1;
       }},
      {"Bell Vector", "Bells",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Fm, BankId::Glass, BankId::Fm, BankId::SineSaw}, {1.f, 0.8f, 0.3f, 0.f},
                 {0, 12, 7, 0});
         vec(r, {0.f, -1.f, 0.f, 0.f, 0.f}, {1.f, 0.f, -1.f, -1.f, -1.f},
                  {0.4f, 1.2f, 1.5f, 1.5f}, 2.0f);
         adsr(r, 0.001f, 4.f, 0.f, 3.f, 0.001f, 2.f, 0.f, 2.f);
         filter(r, 10000.f, 0.f, 0.f);
       }},
      {"Velvet Keys", "Keys",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SineSaw, BankId::Glass, BankId::SineSaw, BankId::Organ},
                 {0.3f, 0.2f, 0.6f, 0.1f}, {0, 12, 0, 0});
         r.inputs.position.value.x = -0.3f;
         r.inputs.vec_depth.value = 0.f;
         adsr(r, 0.003f, 2.5f, 0.3f, 0.5f, 0.003f, 1.f, 0.2f, 0.5f);
         filter(r, 1500.f, 0.1f, 24.f, 0.2f, 0.8f);
         r.inputs.vel_cutoff.value = 24.f;
       }},
      // ---- the second batch: fast loops, morphs, cascades, drift
      {"Morph Arp", "Rhythmic",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SineSaw, BankId::Glass, BankId::Sync, BankId::Fold},
                 {1.f, 0.7f, 0.6f, 0.5f});
         // Round the four corners, a sixteenth each at 120 bpm
         vec(r, {-1.f, 0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, -1.f, 0.f},
             {0.125f, 0.125f, 0.125f, 0.125f}, 0.2f, L::Round, 0, 3);
         adsr(r, 0.003f, 0.3f, 0.9f, 0.3f, 0.003f, 0.3f, 0.5f, 0.3f);
         filter(r, 5000.f, 0.25f, 12.f);
       }},
      {"Vowel Sequencer", "Rhythmic",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Vowels, BankId::Vowels, BankId::Vowels, BankId::Vowels},
                 {0.f, 0.25f, 0.5f, 1.f});
         vec(r, {-1.f, 0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, -1.f, 0.f},
             {0.18f, 0.18f, 0.18f, 0.18f}, 0.3f, L::BackAndForth, 0, 3);
         adsr(r, 0.01f, 0.5f, 1.f, 0.4f, 0.01f, 0.5f, 1.f, 0.4f);
         filter(r, 6000.f, 0.1f, 0.f);
         r.inputs.octave.value = -1;
       }},
      {"Glitch Corners", "Effects",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Digital, BankId::Fold, BankId::Comb, BankId::Sync},
                 {0.8f, 0.9f, 0.6f, 1.f});
         r.inputs.vec_depth.value = 0.f;
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Random, 0.08f, 1.f);
         lfo(r, 2, viceversa::ModTarget::Y, viceversa::LfoShape::Random, 0.08f, 1.f, 90.f);
         adsr(r, 0.002f, 0.4f, 0.8f, 0.2f, 0.002f, 0.4f, 0.5f, 0.2f);
         filter(r, 7000.f, 0.3f, 12.f, 0.6f);
       }},
      {"Breathing Organ", "Keys",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Organ, BankId::Organ, BankId::Organ, BankId::Glass},
                 {0.1f, 0.5f, 1.f, 0.3f}, {0, 0, 0, 12});
         r.inputs.vec_depth.value = 0.f;
         r.inputs.position.value.y = -0.5f;
         r.inputs.pressure_y.value = 1.f;
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Sine, 5.f, 0.5f);
         adsr(r, 0.05f, 0.2f, 1.f, 0.3f, 0.05f, 0.2f, 1.f, 0.3f);
         filter(r, 9000.f, 0.f, 0.f);
       }},
      {"Spectral Drift", "Textures",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::HarmonicSweep, BankId::HarmonicSweep, BankId::HarmonicSweep,
                     BankId::HarmonicSweep},
                 {0.05f, 0.3f, 0.55f, 0.8f}, {0, 7, 12, 19});
         vec(r, {-0.9f, 0.f, 0.9f, 0.f, 0.f}, {0.f, 0.9f, 0.f, -0.9f, 0.f},
             {5.f, 6.f, 5.f, 7.f}, 4.f, L::Round, 0, 3);
         adsr(r, 3.f, 1.f, 1.f, 5.f, 3.f, 1.f, 1.f, 5.f);
         filter(r, 5000.f, 0.15f, 0.f);
         r.inputs.spread.value = 1.f;
       }},
      {"Bell Morph", "Bells",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Fm, BankId::Fm, BankId::Glass, BankId::Fm}, {1.f, 0.6f, 0.9f, 0.2f},
                 {0, 0, 12, -12});
         vec(r, {-1.f, 0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, -1.f, -1.f},
             {0.2f, 0.8f, 1.5f, 2.f}, 2.f);
         adsr(r, 0.001f, 5.f, 0.f, 3.f, 0.001f, 2.f, 0.f, 2.f);
         filter(r, 12000.f, 0.f, 0.f);
       }},
      {"Sync Sweep Lead", "Lead",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Sync, BankId::Sync, BankId::Sync, BankId::Sync},
                 {0.f, 0.35f, 0.7f, 1.f});
         r.inputs.vec_depth.value = 0.f;
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Triangle, 1.2f, 1.f);
         adsr(r, 0.005f, 0.4f, 0.9f, 0.2f, 0.005f, 0.4f, 0.4f, 0.2f);
         filter(r, 4500.f, 0.3f, 18.f, 0.4f);
         r.inputs.play_mode.value = viceversa::PlayMode::Legato;
         r.inputs.glide.value = 0.06f;
       }},
      {"Underwater", "Pad",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Comb, BankId::Vowels, BankId::Comb, BankId::Vowels},
                 {0.2f, 0.9f, 0.8f, 0.1f}, {0, 0, -12, 0});
         r.inputs.vec_depth.value = 0.f;
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Random, 2.5f, 0.6f);
         lfo(r, 2, viceversa::ModTarget::Y, viceversa::LfoShape::Random, 2.5f, 0.6f, 90.f);
         adsr(r, 1.5f, 1.f, 1.f, 3.f, 1.5f, 1.f, 1.f, 3.f);
         filter(r, 900.f, 0.55f, 6.f);
       }},
      {"PD Strings", "Pad",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::PhaseDist, BankId::PhaseDist, BankId::PhaseDist, BankId::PhaseDist},
                 {0.3f, 0.5f, 0.4f, 0.6f}, {0, 0, 0, 0}, {-12.f, -4.f, 5.f, 11.f});
         r.inputs.vec_depth.value = 0.f;
         r.inputs.law.value = viceversa::MixLaw::EqualPower;
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Sine, 7.f, 0.4f);
         lfo(r, 2, viceversa::ModTarget::Y, viceversa::LfoShape::Sine, 7.f, 0.3f, 90.f);
         adsr(r, 0.5f, 1.f, 1.f, 1.2f, 0.6f, 1.f, 0.8f, 1.f);
         filter(r, 4000.f, 0.1f, 12.f);
         r.inputs.spread.value = 1.f;
       }},
      {"Fold Growl", "Bass",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Fold, BankId::Fold, BankId::SquareSaw, BankId::SineSaw},
                 {0.9f, 0.4f, 1.f, 0.f}, {0, 0, 0, -12});
         vec(r, {0.f, -0.8f, 0.f, 0.f, 0.f}, {1.f, 0.f, -0.8f, -0.8f, -0.8f},
             {0.05f, 0.15f, 1.f, 1.f}, 0.1f);
         r.inputs.frame_env.value = 0.4f;
         adsr(r, 0.002f, 0.5f, 0.8f, 0.1f, 0.002f, 0.25f, 0.2f, 0.1f);
         filter(r, 600.f, 0.45f, 36.f, 0.7f);
         r.inputs.octave.value = -1;
         r.inputs.play_mode.value = viceversa::PlayMode::Mono;
       }},
      {"Choir of Glass", "Pad",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Glass, BankId::Vowels, BankId::Glass, BankId::Vowels},
                 {0.6f, 0.3f, 0.95f, 0.7f}, {12, 0, 19, 12});
         vec(r, {-0.6f, 0.f, 0.6f, 0.f, 0.f}, {0.f, 0.6f, 0.f, -0.6f, 0.f},
             {2.f, 2.5f, 2.f, 3.f}, 2.f, L::BackAndForth, 0, 3);
         adsr(r, 1.f, 1.f, 1.f, 2.5f, 1.f, 1.f, 1.f, 2.5f);
         filter(r, 7000.f, 0.f, 0.f);
         r.inputs.spread.value = 1.f;
       }},
      {"Pulse Rider", "Lead",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::PulseWidth, BankId::PulseWidth, BankId::PulseWidth,
                     BankId::PulseWidth},
                 {0.f, 0.5f, 1.f, 0.75f});
         // Pulse-width modulation, by the vector going back and forth
         vec(r, {-1.f, 0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, -1.f, 0.f},
             {0.25f, 0.25f, 0.25f, 0.25f}, 0.2f, L::BackAndForth, 0, 2);
         adsr(r, 0.005f, 0.3f, 0.9f, 0.2f, 0.005f, 0.3f, 0.6f, 0.2f);
         filter(r, 5000.f, 0.2f, 12.f, 0.3f);
         r.inputs.play_mode.value = viceversa::PlayMode::Legato;
         r.inputs.glide.value = 0.04f;
       }},
      {"Octave Cascade", "Rhythmic",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SineSaw, BankId::SineSaw, BankId::SineSaw, BankId::SineSaw},
                 {0.7f, 0.7f, 0.7f, 0.7f}, {0, 12, 24, 36});
         // Each corner an octave up: going round, an arpeggio of octaves
         vec(r, {-1.f, 0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, -1.f, 0.f},
             {0.1f, 0.1f, 0.1f, 0.1f}, 0.2f, L::Round, 0, 3);
         adsr(r, 0.003f, 0.5f, 0.8f, 0.4f, 0.003f, 0.5f, 0.6f, 0.4f);
         filter(r, 8000.f, 0.1f, 6.f);
       }},
      {"Granite", "Percussion",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::Fold, BankId::Digital, BankId::Comb, BankId::Fm},
                 {1.f, 0.7f, 0.5f, 0.9f}, {0, 7, -5, 12});
         r.inputs.vec_depth.value = 0.f;
         // Near the Fold corner: four unrelated waves in equal parts cancel
         r.inputs.position.value.x = -0.6f;
         r.inputs.position.value.y = 0.15f;
         adsr(r, 0.001f, 0.4f, 0.f, 0.25f, 0.001f, 0.15f, 0.f, 0.1f);
         filter(r, 4000.f, 0.4f, 36.f, 0.8f);
       }},
      {"Night Drone", "Textures",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SineSaw, BankId::Comb, BankId::Vowels, BankId::HarmonicSweep},
                 {0.4f, 0.3f, 0.6f, 0.2f}, {-12, 7, 0, 12}, {-6.f, 4.f, 0.f, 7.f});
         vec(r, {-0.8f, 0.2f, 0.7f, -0.1f, 0.f}, {0.1f, 0.7f, -0.3f, -0.8f, 0.f},
             {8.f, 9.f, 7.f, 10.f}, 6.f, L::BackAndForth, 0, 3);
         lfo(r, 1, viceversa::ModTarget::X, viceversa::LfoShape::Random, 4.f, 0.1f);
         lfo(r, 2, viceversa::ModTarget::Y, viceversa::LfoShape::Random, 4.f, 0.1f, 90.f);
         adsr(r, 4.f, 1.f, 1.f, 6.f, 4.f, 1.f, 1.f, 6.f);
         filter(r, 2500.f, 0.3f, 6.f);
         r.inputs.spread.value = 1.f;
       }},
      {"Wah Keys", "Keys",
       [](viceversa::ViceVersa& r) {
         sources(r, {BankId::SquareSaw, BankId::PhaseDist, BankId::SineSaw, BankId::Organ},
                 {0.6f, 0.4f, 0.8f, 0.3f});
         r.inputs.vec_depth.value = 0.f;
         r.inputs.position.value.x = -0.3f;
         adsr(r, 0.003f, 1.5f, 0.4f, 0.3f, 0.003f, 0.35f, 0.1f, 0.3f);
         filter(r, 500.f, 0.65f, 40.f, 0.3f, 0.6f);
         r.inputs.vel_cutoff.value = 30.f;
       }},
  };

  for(const auto& p : presets)
  {
    auto r = std::make_unique<viceversa::ViceVersa>();
    r->inputs.vec_env.value = viceversa::VecEnvSpec::centred().points();
    p.set(*r);
    write(dir, p.name, p.category, *r,
          r->inputs.level);
  }
}
}

int main(int argc, char** argv)
{
  if(argc < 2)
  {
    std::cerr << "usage: " << argv[0] << " <Presets folder>\n";
    return 1;
  }
  try
  {
    const std::filesystem::path root = argv[1];
    czumarzuPresets(root);
    viceVersaPresets(root);
  }
  catch(const std::exception& e)
  {
    std::cerr << argv[0] << ": " << e.what() << "\n";
    return 1;
  }
}
