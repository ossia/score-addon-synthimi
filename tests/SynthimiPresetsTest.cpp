// The presets the tool writes for CZumarzu and ViceVersa: each loads back,
// by the ports' names, onto the ports it was taken from, and plays at the
// level it was matched to.
#include <tools/SynthimiPresets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <map>
#include <random>

namespace
{
//! The value of a preset's entry on a port, as the host would set it; false
//! if the value is not of the port's type
template <typename Port>
bool load(Port& port, const QJsonObject& o)
{
  using V = std::remove_cvref_t<decltype(port.value)>;
  if constexpr(requires { port.sync; })
  {
    // A time chooser: [seconds, 0]
    const auto a = o["Vec2f"].toArray();
    if(a.size() != 2 || !a[0].isDouble() || a[1].toDouble() != 0.)
      return false;
    port.value = V(a[0].toDouble());
    port.sync = false;
    return true;
  }
  else if constexpr(std::is_enum_v<V>)
  {
    if(!o["Int"].isDouble())
      return false;
    port.value = V(o["Int"].toInt());
    return true;
  }
  else if constexpr(std::is_same_v<V, bool>)
  {
    if(!o["Bool"].isBool())
      return false;
    port.value = o["Bool"].toBool();
    return true;
  }
  else if constexpr(std::is_integral_v<V>)
  {
    if(!o["Int"].isDouble())
      return false;
    port.value = V(o["Int"].toInt());
    return true;
  }
  else if constexpr(std::is_floating_point_v<V>)
  {
    if(!o["Float"].isDouble())
      return false;
    port.value = V(o["Float"].toDouble());
    return true;
  }
  else if constexpr(requires { port.value.x; port.value.y; })
  {
    const auto a = o["Vec2f"].toArray();
    if(a.size() != 2)
      return false;
    port.value.x = float(a[0].toDouble());
    port.value.y = float(a[1].toDouble());
    return true;
  }
  else if constexpr(std::is_same_v<V, std::vector<float>>)
  {
    if(!o["Tuple"].isArray())
      return false;
    port.value.clear();
    for(const auto& e : o["Tuple"].toArray())
    {
      if(!e.toObject()["Float"].isDouble())
        return false;
      port.value.push_back(float(e.toObject()["Float"].toDouble()));
    }
    return true;
  }
  else
    static_assert(sizeof(V) == 0, "port type without a decoding");
}

/*! The level as the tool means it, measured here on its own: middle C at
 *  velocity 100, played through the processor as a host does, and the
 *  loudest 100 ms of 8 s, RMS over both channels, in dBFS; and the peak
 */
template <typename Proc>
std::pair<double, double> playedLevel(Proc& proc)
{
  constexpr int frames = 480, window = 4800, rate = 48000;
  std::vector<float> l(frames), r(frames);
  float* channels[2]{l.data(), r.data()};
  proc.prepare(halp::setup{.input_channels = 0, .output_channels = 2, .frames = frames, .rate = rate});
  proc.outputs.audio.samples = channels;
  double loudest = 0., sum = 0., peak = 0.;
  int inWindow = 0;
  for(int b = 0; b < 8 * rate / frames; b++)
  {
    proc.inputs.midi.midi_messages.clear();
    if(b == 0)
      proc.inputs.midi.midi_messages.push_back(halp::midi_msg{{0x90, 60, 100}, 0});
    proc(halp::tick{frames});
    for(int i = 0; i < frames; i++)
    {
      const double a = l[std::size_t(i)], c = r[std::size_t(i)];
      peak = std::max({peak, std::abs(a), std::abs(c)});
      sum += 0.5 * (a * a + c * c);
      if(++inWindow == window)
      {
        loudest = std::max(loudest, sum / window);
        sum = 0.;
        inWindow = 0;
      }
    }
  }
  return {10. * std::log10(std::max(loudest, 1e-12)), peak};
}

struct Entry
{
  int index;
  QJsonObject value;
};

//! Every preset of a synth's folder, loaded onto a new processor by name
template <typename Proc>
int checkFolder(const std::filesystem::path& dir)
{
  int count = 0;
  for(const auto& file : std::filesystem::directory_iterator(dir))
  {
    if(file.path().extension() != ".scp")
      continue;
    count++;
    const auto name = file.path().stem().string();
    INFO("preset " << name);
    QFile f(QString::fromStdString(file.path().string()));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(f.readAll()).object();
    CHECK(doc["Key"].toObject()["Uuid"].toString().toStdString() == std::string(Proc::uuid()));
    CHECK(doc["Name"].toString().toStdString() == name);

    const auto preset = doc["Preset"].toArray();
    std::map<std::string, Entry> byName;
    for(const auto& e : preset)
    {
      const auto a = e.toArray();
      REQUIRE(a.size() == 3);
      const auto port = a[2].toString().toStdString();
      INFO("port " << port);
      CHECK(byName.emplace(port, Entry{a[0].toInt(), a[1].toObject()}).second);
    }

    // Each port with a value is in the preset once, under its name, at its
    // index: matched by either, it gets the same value
    auto proc = std::make_unique<Proc>();
    int index = 0, ports = 0;
    avnd::input_introspection<Proc>::for_all(proc->inputs, [&](auto& port) {
      if constexpr(requires { port.value; })
      {
        ports++;
        const std::string portName{std::remove_cvref_t<decltype(port)>::name()};
        INFO("port " << portName);
        const auto it = byName.find(portName);
        CHECK(it != byName.end());
        if(it != byName.end())
        {
          CHECK(it->second.index == index);
          CHECK(load(port, it->second.value));
        }
      }
      index++;
    });
    CHECK(int(preset.size()) == ports);

    // Measured here, through the processor, then by the tool's measure()
    const auto [played, peak] = playedLevel(*proc);
    const double db = measure(*proc);
    INFO("at " << played << " dBFS played, " << db << " dBFS by the tool, peak " << peak);
    CHECK(std::abs(played - kTargetDb) <= kToleranceDb);
    CHECK(std::abs(db - kTargetDb) <= kToleranceDb);
    CHECK(peak <= 1.);
  }
  return count;
}
}

TEST_CASE("The presets load back onto their ports, at their level", "[synthimi][presets]")
{
  const auto root = std::filesystem::temp_directory_path()
                    / ("score_addon_synthimi_presets_test_" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(root);
  czumarzuPresets(root);
  viceVersaPresets(root);

  CHECK(checkFolder<czumarzu::CZumarzu>(root / "CZumarzu") > 0);
  CHECK(checkFolder<viceversa::ViceVersa>(root / "ViceVersa") > 0);
  std::filesystem::remove_all(root);
}
