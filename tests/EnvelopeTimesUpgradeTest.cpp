// Kaboom and Minibang as documents saved them when their envelope times were
// log knobs: each of those knobs had a port type of its own (the avnd custom
// float control of that field), which no longer exists now that they are time
// choosers. The documents load, and every such control comes back as a time
// chooser holding the saved time, in seconds; the other controls are as saved.

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Process/Process.hpp>
#include <Process/ProcessList.hpp>

#include <score/plugins/SerializableHelpers.hpp>
#include <score/serialization/JSONVisitor.hpp>

#include <core/document/Document.hpp>
#include <core/document/DocumentModel.hpp>

#include <ossia/network/value/value_conversion.hpp>

#include <QFile>

#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Document.hpp>

#include <memory>
#include <set>

namespace
{
QByteArray fixture(const char* name)
{
  QFile f{QStringLiteral(SYNTHIMI_TEST_DATA_DIR "/") + QString::fromUtf8(name)};
  if(!f.open(QIODevice::ReadOnly))
    return {};
  return f.readAll();
}

template <typename T>
UuidKey<T> key_of(const rapidjson::Value& uuid)
{
  UuidKey<T> k;
  JSONWriter wr{uuid};
  wr.writeTo(k);
  return k;
}

void check_upgrade(
    const score::GUIApplicationContext& ctx, const char* file,
    const std::set<QString>& time_choosers, std::size_t expected_count)
{
  const QByteArray bytes = fixture(file);
  REQUIRE(!bytes.isEmpty());
  const auto json = readJson(bytes);
  REQUIRE(json.IsObject());

  auto& processes = ctx.interfaces<Process::ProcessFactoryList>();
  if(!processes.get(key_of<Process::ProcessModel>(json["uuid"])))
    SKIP("The object is not built");

  auto* doc = score::test::new_document(ctx);
  REQUIRE(doc);

  JSONObject::Deserializer des{json};
  std::unique_ptr<Process::ProcessModel> loaded{
      deserialize_interface(processes, des, doc->context(), &doc->model())};
  REQUIRE(loaded);

  const auto& ports = ctx.interfaces<Process::PortFactoryList>();
  const auto& saved_inlets = json["Inlets"].GetArray();
  REQUIRE(loaded->inlets().size() == saved_inlets.Size());

  std::size_t upgraded = 0;
  std::size_t changed_from_default = 0;
  for(const auto& saved : saved_inlets)
  {
    const auto id = Id<Process::Port>{saved["id"].GetInt()};
    auto* inlet = loaded->inlet(id);
    REQUIRE(inlet);
    if(!saved.HasMember("Value") || !saved.HasMember("Custom"))
      continue;

    const auto name = JsonValue{saved["Custom"]}.toString();
    const ossia::value saved_value
        = JSONWriter::unmarshall<ossia::value>(saved["Value"]);
    INFO(file << ": " << name.toStdString());

    if(time_choosers.contains(name))
    {
      // The saved port type is the old knob's own
      CHECK(!ports.get(key_of<Process::Port>(saved["uuid"])));

      auto* chooser = qobject_cast<Process::TimeChooser*>(inlet);
      REQUIRE(chooser);
      CHECK(
          chooser->value()
          == ossia::value{ossia::vec2f{ossia::convert<float>(saved_value), 0.f}});
      if(saved_value != JSONWriter::unmarshall<ossia::value>(saved["Init"]))
        changed_from_default++;
      upgraded++;
    }
    else if(auto* control = qobject_cast<Process::ControlInlet*>(inlet))
    {
      CHECK(control->value() == saved_value);
    }
  }
  CHECK(upgraded == expected_count);
  // The fixtures come from documents where these were edited
  CHECK(changed_from_default > 0);
}
}

TEST_CASE("Kaboom: decays saved as log knobs load as time choosers", "[synthimi]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    // Eight drum channels, eight times each
    check_upgrade(
        ctx, "kaboom-before-time-choosers.json",
        {"P. Decay", "Decay", "Shake", "FM I.Dec", "Flam Time", "N. Decay", "Attack",
         "A. Decay"},
        64);
  });
}

TEST_CASE(
    "Minibang: envelope times saved as log knobs load as time choosers", "[synthimi]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    check_upgrade(
        ctx, "minibang-before-time-choosers.json",
        {"P. Attack", "P. Decay", "P. Release", "F. Attack", "F. Decay", "F. Release",
         "Attack", "Decay", "Release"},
        9);
  });
}
