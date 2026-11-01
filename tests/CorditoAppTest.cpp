// Cordito inside the application: its ports are where the presets expect them,
// its layer builds and switches engine pages, and every preset of the
// default package loads through score's preset code and plays sanely.
#include <Cordito/Cordito.hpp>

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Process/DocumentPlugin.hpp>
#include <Process/Focus/FocusDispatcher.hpp>
#include <Process/Preset.hpp>
#include <Process/Process.hpp>
#include <Process/ProcessContext.hpp>
#include <Process/ProcessList.hpp>

#include <score/graphics/RectItem.hpp>
#include <score/graphics/widgets/QGraphicsKnob.hpp>

#include <ossia/network/value/value_conversion.hpp>

#include <avnd/introspection/input.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGraphicsScene>

#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Document.hpp>
#include <score_test/Project.hpp>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
constexpr auto kUuid = "3c8e5f1a-9d27-4b6e-a0c4-7f2d18e95b36";

// The preset format stores values by port index: this is the frozen order
constexpr std::array<std::string_view, 62> kPorts{
    "Input",        "Exciter",      "Hardness",    "Position",   "Force",
    "Noise",        "Engine",       "Tune",        "Fine",       "Decay",
    "Brightness",   "Stiffness",    "Tension",     "Detune",     "Material",
    "Modes",        "Buzz",         "Bow Speed",   "Bow Attack", "Bow Lift",
    "Release",
    "Mute",         "Damper Noise", "Symp. Level", "Symp. Tuning", "Symp. Root",
    "Symp. Decay",  "Body",         "Body Size",   "Body Mix",   "Coupling",
    "Play Mode",    "Glide",        "Bend Range",  "Vel->Force", "Pressure",
    "Spread",       "Level",        "Voices",      "Coupling Detune",
    "LFO 1 Target", "LFO 1 Shape",  "LFO 1 Period", "LFO 1 Depth", "LFO 1 Delay",
    "LFO 2 Target", "LFO 2 Shape",  "LFO 2 Period", "LFO 2 Depth", "LFO 2 Delay",
    "LFO 3 Target", "LFO 3 Shape",  "LFO 3 Period", "LFO 3 Depth", "LFO 3 Delay",
    "LFO 4 Target", "LFO 4 Shape",  "LFO 4 Period", "LFO 4 Depth", "LFO 4 Delay",
    "String Level", "Exciter Level"};

Process::ProcessModel* addCordito(const score::GUIApplicationContext&, score::Document& doc)
{
  return score::test::add_process(doc, kUuid, {});
}

double number(const ossia::value& v)
{
  if(auto* t = v.target<ossia::vec2f>())
    return (*t)[0]; // a time chooser: seconds, and its sync flag
  return ossia::convert<double>(v);
}

//! The DSP parameters from the ports, through the processor's own mapping:
//! the control inlets, in order, are its parameter inputs, which the port
//! order test checks
crd::Params paramsOf(const Process::ProcessModel& proc)
{
  std::vector<double> values;
  for(auto* port : proc.inlets())
    if(auto* c = qobject_cast<Process::ControlInlet*>(port))
      values.push_back(number(c->value()));
  crd::Cordito cordito;
  std::size_t i = 0;
  avnd::parameter_input_introspection<crd::Cordito>::for_all(
      cordito.inputs, [&](auto& field) {
    using T = std::decay_t<decltype(field.value)>;
    const double x = i < values.size() ? values[i] : 0.;
    i++;
    if constexpr(std::is_enum_v<T>)
      field.value = T(int(x));
    else if constexpr(std::is_same_v<T, bool>)
      field.value = x != 0.;
    else
      field.value = static_cast<T>(x);
  });
  REQUIRE(i == values.size());
  return cordito.params();
}

int visibleKnobs(QGraphicsScene& scene)
{
  int n = 0;
  for(auto* item : scene.items())
    if(auto* k = dynamic_cast<score::QGraphicsKnob*>(item); k && k->isVisible())
      n++;
  return n;
}

QString presetDir()
{
  if(auto env = qEnvironmentVariable("CORDITO_PRESET_DIR"); !env.isEmpty())
    return env;
  return QDir::homePath() + "/Documents/ossia/score/packages/default/Presets/Cordito";
}
}

TEST_CASE("Cordito: the ports are in the order the presets use", "[synthimi][cordito]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto* proc = addCordito(ctx, *doc);
    if(!proc)
      SKIP("Cordito is not built");
    REQUIRE(proc->inlets().size() == kPorts.size());
    for(std::size_t i = 0; i < kPorts.size(); i++)
    {
      INFO("port " << i);
      CHECK(proc->inlets()[i]->id().val() == int(i));
      CHECK(proc->inlets()[i]->name().toStdString() == kPorts[i]);
    }
    REQUIRE(proc->outlets().size() == 1);
  });
}

TEST_CASE("Cordito: the layer builds and follows the engine", "[synthimi][cordito]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto* proc = addCordito(ctx, *doc);
    if(!proc)
      SKIP("Cordito is not built");
    Process::DataflowManager dfm;
    FocusDispatcher fd;
    Process::Context pctx{doc->context(), dfm, fd};
    auto* factory = ctx.interfaces<Process::LayerFactoryList>().findDefaultFactory(*proc);
    REQUIRE(factory);
    QGraphicsScene scene;
    auto* ui = factory->makeItem(*proc, pctx, nullptr);
    REQUIRE(ui);
    scene.addItem(ui);
    QApplication::processEvents();

    Process::ControlInlet* engine{};
    for(auto* port : proc->inlets())
      if(port->name() == "Engine")
        engine = qobject_cast<Process::ControlInlet*>(port);
    REQUIRE(engine);

    // The header and the first tab, String: its 12 knobs (Decay is a time
    // chooser), Buzz (disabled on Pluck), and the mix: String, Exciter, Symp.
    // (disabled without sympathetic strings) and Level. Bow swaps Buzz for Bow
    // Speed (Bow Attack and Bow Lift are time choosers, not knobs), Struck
    // swaps it for Modes, a spin box.
    const int pluck = visibleKnobs(scene);
    CHECK(pluck == 17);
    engine->setValue(int(crd::Engine::Bow));
    QApplication::processEvents();
    CHECK(visibleKnobs(scene) == pluck);
    engine->setValue(int(crd::Engine::Struck));
    QApplication::processEvents();
    CHECK(visibleKnobs(scene) == pluck - 1);
  });
}

TEST_CASE("Cordito: every preset loads and plays", "[synthimi][cordito]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    const QDir dir{presetDir()};
    const auto files = dir.entryList({"*.scp"}, QDir::Files, QDir::Name);
    if(files.isEmpty())
      SKIP("No Cordito preset in " << presetDir().toStdString());

    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto* proc = addCordito(ctx, *doc);
    if(!proc)
      SKIP("Cordito is not built");
    auto& processes = ctx.interfaces<Process::ProcessFactoryList>();

    for(const auto& file : files)
    {
      INFO(file.toStdString());
      QFile f{dir.filePath(file)};
      REQUIRE(f.open(QIODevice::ReadOnly));
      const auto preset = Process::Preset::fromJson(processes, f.readAll());
      REQUIRE(preset);
      CHECK(preset->key.key == proc->concreteKey());
      CHECK(!preset->category.isEmpty());
      proc->loadPreset(*preset);

      // Every value inside its port's range. A combo box's domain is its list
      // of values, not a numeric range.
      for(auto* port : proc->inlets())
        if(auto* c = qobject_cast<Process::ControlInlet*>(port);
           c && !qobject_cast<Process::ComboBox*>(c) && !qobject_cast<Process::Enum*>(c))
        {
          INFO(c->name().toStdString());
          const double v = number(c->value());
          CHECK(std::isfinite(v));
          CHECK(v >= double(c->domain().get().convert_min<float>()) - 1e-4);
          CHECK(v <= double(c->domain().get().convert_max<float>()) + 1e-4);
        }

      // A phrase across the range: finite, audible, and below the limiter
      const crd::Params p = paramsOf(*proc);
      auto synth = std::make_unique<crd::Synth>();
      synth->prepare(48000.);
      std::vector<double> l(24000), r(24000);
      double energy = 0.;
      bool finite = true;
      for(int note : {43, 55, 62, 67, 74})
      {
        const unsigned char on[3]{0x90, (unsigned char)note, 100};
        synth->midi(on, 3, p);
        synth->render(p, l.data(), r.data(), int(l.size()));
        for(std::size_t i = 0; i < l.size(); i++)
        {
          finite &= std::isfinite(l[i]) && std::isfinite(r[i]);
          energy += l[i] * l[i] + r[i] * r[i];
        }
        const unsigned char off[3]{0x80, (unsigned char)note, 0};
        synth->midi(off, 3, p);
      }
      const double rms = std::sqrt(energy / (2. * 5. * double(l.size())));
      CHECK(finite);
      CHECK(rms > 0.003);
      CHECK(synth->preLimiterPeak < 1.5);
    }
  });
}
