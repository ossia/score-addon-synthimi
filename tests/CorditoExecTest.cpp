// Cordito through score's execution, as the user plays it: controls set on
// the process (from the UI, a preset, a document), MIDI into its inlet, audio
// out of its node. The DSP tests feed the synth directly and miss what lies
// between: preset loading, the controls' values and their conversion.
#include <Cordito/Cordito.hpp>

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Process/Preset.hpp>
#include <Process/ProcessList.hpp>

#include <Scenario/Document/Interval/IntervalExecution.hpp>
#include <Scenario/Document/Interval/IntervalModel.hpp>

#include <Execution/BaseScenarioComponent.hpp>
#include <Execution/DocumentPlugin.hpp>

#include <score/document/DocumentInterface.hpp>

#include <core/document/Document.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/detail/thread.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>

#include <Crousti/Executor.hpp>
#include <Crousti/ProcessModel.hpp>
#include <catch2/catch_test_macros.hpp>
#include <libremidi/ump_events.hpp>
#include <rapidjson/document.h>
#include <score_test/App.hpp>
#include <score_test/Document.hpp>
#include <score_test/Project.hpp>

#include <cmath>
#include <vector>

namespace
{
constexpr auto kUuid = "3c8e5f1a-9d27-4b6e-a0c4-7f2d18e95b36";

QString presetDir()
{
  if(auto env = qEnvironmentVariable("CORDITO_PRESET_DIR"); !env.isEmpty())
    return env;
  return QDir::homePath() + "/Documents/ossia/score/packages/default/Presets/Cordito";
}

Process::ControlInlet& control(Process::ProcessModel& p, const QString& name)
{
  for(auto* port : p.inlets())
    if(auto* c = qobject_cast<Process::ControlInlet*>(port); c && c->name() == name)
      return *c;
  FAIL("no control named " << name.toStdString());
  throw;
}

void runCommands(Execution::DocumentPlugin& plug)
{
  ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
  plug.runAllCommands();
  ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
}

//! The process's node in the document's execution, run block by block
struct Running
{
  Execution::DocumentPlugin& plug;
  std::shared_ptr<ossia::graph_node> node;
  static constexpr int block = 64;
  int64_t frame{};

  crd::Cordito& object()
  {
    return static_cast<oscr::safe_node<crd::Cordito>&>(*node).impl.effect;
  }

  ossia::midi_port& midi()
  {
    for(auto* in : node->root_inputs())
      if(auto* m = in->target<ossia::midi_port>())
        return *m;
    FAIL("no MIDI inlet");
    throw;
  }

  //! A MIDI 1 message, converted to the MIDI 2 packets score carries
  void send(uint8_t a, uint8_t b, uint8_t c)
  {
    libremidi::midi1_to_midi2 conv;
    const uint8_t bytes[3]{a, b, c};
    conv.convert_packets(bytes, 3, 0, [&](const libremidi::ump& u) {
      midi().messages.push_back(u);
      return std::errc{};
    });
  }
  void note(int n, int vel) { send(uint8_t(vel > 0 ? 0x90 : 0x80), uint8_t(n), uint8_t(vel)); }

  //! `seconds` of the left channel
  std::vector<double> run(double seconds)
  {
    runCommands(plug);
    auto& st = *plug.context().execState;
    std::vector<double> out;
    const int blocks = int(seconds * st.sampleRate / block);
    constexpr double fps = ossia::flicks_per_second<double>;
    for(int b = 0; b < blocks; b++)
    {
      ossia::token_request tk{};
      tk.prev_date = ossia::time_value{int64_t(double(frame) * fps / st.sampleRate)};
      tk.date = ossia::time_value{int64_t(double(frame + block) * fps / st.sampleRate)};
      tk.start_sample = 0;
      tk.length_sample = block;
      tk.speed = 1.;
      ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
      node->run(tk, ossia::exec_state_facade{&st});
      ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      frame += block;
      midi().messages.clear();
      auto& a = *node->root_outputs()[0]->target<ossia::audio_port>();
      REQUIRE(a.channels() == 2);
      out.insert(out.end(), a.channel(0).begin(), a.channel(0).begin() + block);
    }
    return out;
  }
};

template <typename F>
void withCordito(F&& f)
{
  score::test::run_in_app([&](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto* proc = score::test::add_process(*doc, kUuid, {});
    if(!proc)
      SKIP("Cordito is not built");
    auto& plug = doc->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*doc));
    runCommands(plug);
    REQUIRE(plug.context().execState);
    plug.context().execState->bufferSize = Running::block;
    REQUIRE(plug.baseScenario());
    auto& procs = plug.baseScenario()->baseInterval().processes();
    auto it = procs.find(proc->id());
    REQUIRE(it != procs.end());
    REQUIRE(it->second->node);
    Running r{plug, it->second->node};
    f(ctx, *proc, r);
  });
}

double goertzel(const std::vector<double>& x, double fs, double f, double t0, double t1)
{
  const auto a = std::size_t(t0 * fs), b = std::min(x.size(), std::size_t(t1 * fs));
  const double w = 2. * M_PI * f / fs, c = 2. * std::cos(w);
  double s1 = 0., s2 = 0.;
  for(auto i = a; i < b; i++)
  {
    const double h = 0.5 - 0.5 * std::cos(2. * M_PI * double(i - a) / double(b - a));
    const double s0 = x[i] * h + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(std::max(0., s1 * s1 + s2 * s2 - c * s1 * s2));
}

//! How far the pitch moves over [t0, t1], in cents, from 40 ms windows
double pitchSwing(const std::vector<double>& x, double fs, double f0, double t0, double t1)
{
  double lo = 1e9, hi = -1e9;
  for(double t = t0; t + 0.04 <= t1; t += 0.02)
  {
    double best = f0, bv = -1.;
    for(int i = -100; i <= 100; i++)
    {
      const double f = f0 * std::exp2(i / 1200.);
      if(const double v = goertzel(x, fs, f, t, t + 0.04); v > bv)
      {
        bv = v;
        best = f;
      }
    }
    const double c = 1200. * std::log2(best / f0);
    lo = std::min(lo, c);
    hi = std::max(hi, c);
  }
  return hi - lo;
}
}

TEST_CASE("Cordito: a preset sets every control, its LFOs included", "[synthimi][cordito]")
{
  // Loading a preset sets only the controls it stores: one without the LFO
  // ports left the LFOs of the previous preset running
  withCordito([](const score::GUIApplicationContext& ctx, Process::ProcessModel& proc,
                 Running& r) {
    auto& processes = ctx.interfaces<Process::ProcessFactoryList>();
    auto load = [&](const QString& name) {
      QFile f{presetDir() + "/" + name + ".scp"};
      if(!f.open(QIODevice::ReadOnly))
        SKIP("no preset " << name.toStdString() << " in " << presetDir().toStdString());
      const auto preset = Process::Preset::fromJson(processes, f.readAll());
      REQUIRE(preset);
      proc.loadPreset(*preset);
      r.run(0.01);
    };
    load("Late Vibrato");
    auto& in = r.object().inputs;
    CHECK(in.lfo1_target.value == crd::LfoTarget::Pitch);
    CHECK(std::abs(in.lfo1_delay.value - 0.6f) < 1e-4f);
    CHECK(in.engine.value == crd::Engine::Bow);

    load("Lantern Thread");
    CHECK(in.lfo1_target.value == crd::LfoTarget::Off);
    CHECK(in.engine.value == crd::Engine::Pluck);

    // Every preset stores every port
    const QDir dir{presetDir()};
    for(const auto& file : dir.entryList({"*.scp"}, QDir::Files))
    {
      QFile f{dir.filePath(file)};
      REQUIRE(f.open(QIODevice::ReadOnly));
      const auto preset = Process::Preset::fromJson(processes, f.readAll());
      REQUIRE(preset);
      rapidjson::Document doc;
      doc.Parse(preset->data.constData(), std::size_t(preset->data.size()));
      INFO(file.toStdString());
      REQUIRE(doc.IsArray());
      CHECK(int(doc.GetArray().Size()) == crd::kPresetPorts);
    }
  });
}

TEST_CASE("Cordito: a vibrato set from the controls waits its Delay", "[synthimi][cordito]")
{
  withCordito([](const score::GUIApplicationContext&, Process::ProcessModel& proc, Running& r) {
    // As the inspector sets them: a combo box's value, {seconds, 0} for a
    // time chooser
    control(proc, "Engine").setValue(int(crd::Engine::Bow));
    control(proc, "LFO 1 Target").setValue(int(crd::LfoTarget::Pitch));
    control(proc, "LFO 1 Shape").setValue(int(crd::LfoShape::Sine));
    control(proc, "LFO 1 Period").setValue(ossia::vec2f{0.25f, 0.f});
    control(proc, "LFO 1 Depth").setValue(0.5f);
    control(proc, "LFO 1 Delay").setValue(ossia::vec2f{1.f, 0.f});
    r.run(0.01);
    REQUIRE(r.object().inputs.lfo1_target.value == crd::LfoTarget::Pitch);
    REQUIRE(r.object().inputs.lfo1_delay.value == 1.f);

    r.note(60, 100);
    const auto l = r.run(2.4);
    const double fs = r.object().synth.sampleRate(), f0 = 440. * std::exp2((60 - 69) / 12.);
    double e = 0.;
    for(double x : l)
      e += x * x;
    INFO("synth at " << fs << " Hz, execution at " << r.plug.context().execState->sampleRate
                     << " Hz, " << l.size() << " samples, rms " << std::sqrt(e / double(l.size()))
                     << ", " << r.object().synth.activeVoices() << " voices");
    REQUIRE(std::sqrt(e / double(l.size())) > 1e-3);
    INFO("first second " << pitchSwing(l, fs, f0, 0.3, 0.95) << " cents, then "
                         << pitchSwing(l, fs, f0, 1.6, 2.4));
    CHECK(pitchSwing(l, fs, f0, 0.3, 0.95) < 6.);
    CHECK(pitchSwing(l, fs, f0, 1.6, 2.4) > 70.);
  });
}

namespace
{
//! The strongest pitch near f0 over [t0, t1], in cents
double pitchOf(const std::vector<double>& x, double fs, double f0, double t0, double t1)
{
  double best = f0, bv = -1.;
  for(int i = -300; i <= 300; i++)
  {
    const double f = f0 * std::exp2(i / 1200.);
    if(const double v = goertzel(x, fs, f, t0, t1); v > bv)
    {
      bv = v;
      best = f;
    }
  }
  return 1200. * std::log2(best / f0);
}

double rmsOf(const std::vector<double>& x, double fs, double t0, double t1)
{
  double e = 0.;
  const auto a = std::size_t(t0 * fs), b = std::min(x.size(), std::size_t(t1 * fs));
  for(auto i = a; i < b; i++)
    e += x[i] * x[i];
  return std::sqrt(e / double(std::max<std::size_t>(1, b - a)));
}
}

TEST_CASE("Cordito: every engine, chosen from its control, plays in tune", "[synthimi][cordito]")
{
  withCordito([](const score::GUIApplicationContext&, Process::ProcessModel& proc, Running& r) {
    const double f0 = 440. * std::exp2((60 - 69) / 12.);
    for(auto e : {crd::Engine::Pluck, crd::Engine::Struck, crd::Engine::Bow, crd::Engine::Buzz,
                  crd::Engine::Friction})
    {
      control(proc, "Engine").setValue(int(e));
      r.run(0.01);
      REQUIRE(r.object().inputs.engine.value == e);
      r.note(60, 100);
      const auto l = r.run(1.2);
      r.note(60, 0);
      r.run(1.5);
      const double fs = r.object().synth.sampleRate();
      INFO("engine " << int(e) << ": rms " << rmsOf(l, fs, 0.5, 1.1) << ", "
                     << pitchOf(l, fs, f0, 0.5, 1.1) << " cents");
      CHECK(rmsOf(l, fs, 0.5, 1.1) > 1e-3);
      CHECK(std::abs(pitchOf(l, fs, f0, 0.5, 1.1)) < 10.);
    }
  });
}

TEST_CASE("Cordito: pitch bend and the sustain pedal reach it through score", "[synthimi][cordito]")
{
  withCordito([](const score::GUIApplicationContext&, Process::ProcessModel& proc, Running& r) {
    control(proc, "Engine").setValue(int(crd::Engine::Bow));
    control(proc, "Bend Range").setValue(2);
    r.run(0.01);
    const double f0 = 440. * std::exp2((60 - 69) / 12.);
    r.note(60, 100);
    r.run(0.6);
    // Bend fully up: two semitones at a Bend Range of 2
    r.send(0xE0, 0x7F, 0x7F);
    const auto bent = r.run(0.8);
    const double fs = r.object().synth.sampleRate();
    INFO("bent " << pitchOf(bent, fs, f0 * std::exp2(2. / 12.), 0.3, 0.8) << " cents from +2 st");
    CHECK(std::abs(pitchOf(bent, fs, f0 * std::exp2(2. / 12.), 0.3, 0.8)) < 15.);
    r.send(0xE0, 0x00, 0x40);

    // Sustain down, note off: the bow keeps playing; pedal up, it stops
    r.send(0xB0, 64, 127);
    r.note(60, 0);
    const auto held = r.run(0.6);
    CHECK(rmsOf(held, fs, 0.3, 0.6) > 1e-3);
    r.send(0xB0, 64, 0);
    r.run(2.);
    CHECK(r.object().synth.activeVoices() == 0);
  });
}
