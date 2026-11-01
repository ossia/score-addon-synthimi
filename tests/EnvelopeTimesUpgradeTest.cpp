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

#include <score/model/EntitySerialization.hpp>
#include <score/plugins/SerializableHelpers.hpp>
#include <score/serialization/JSONVisitor.hpp>

#include <core/document/Document.hpp>
#include <core/document/DocumentModel.hpp>

#include <ossia/detail/flat_set.hpp>

#include <ossia/network/value/value_conversion.hpp>

#include <QFile>

#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Document.hpp>

#include <memory>

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
    const ossia::flat_set<QString>& time_choosers, std::size_t expected_count,
    std::size_t added_controls)
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
  REQUIRE(loaded->inlets().size() == saved_inlets.Size() + added_controls);
  // Controls added since come after the saved ones, at their defaults
  for(std::size_t i = saved_inlets.Size(); i < loaded->inlets().size(); i++)
  {
    auto* amount = qobject_cast<Process::ControlInlet*>(loaded->inlets()[i]);
    REQUIRE(amount);
    CHECK(amount->name() == QStringLiteral("Pitch Env Amount (st)"));
    CHECK(amount->value() == ossia::value{24.f});
  }

  std::size_t upgraded = 0;
  std::size_t changed_from_default = 0;
  for(const auto& saved : saved_inlets)
  {
    // Same id, same port: cables and presets refer to ports by id
    const auto id = Id<Process::Port>{saved["id"].GetInt()};
    auto* inlet = loaded->inlet(id);
    REQUIRE(inlet);
    if(!saved.HasMember("Value") || !saved.HasMember("Custom"))
      continue;

    const auto name = JsonValue{saved["Custom"]}.toString();
    CHECK(inlet->name() == name);
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
        64, 0);
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
        9, 1);
  });
}

TEST_CASE(
    "Kabang: the pitch amounts come after every older port, which keep their ids",
    "[synthimi]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto& processes = ctx.interfaces<Process::ProcessFactoryList>();
    auto* factory = processes.get(UuidKey<Process::ProcessModel>::fromString(
        QStringLiteral("d6cd303e-c851-4655-806e-6c344cade2ae")));
    if(!factory)
      SKIP("Kabang is not built");
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    std::unique_ptr<Process::ProcessModel> original{factory->make(
        TimeVal::fromMsecs(1000.), {}, Id<Process::ProcessModel>{0},
        doc->context(), &doc->model())};
    REQUIRE(original);

    int channel = 0;
    for(auto* port : original->inlets())
      if(port->name() == QStringLiteral("Pitch"))
      {
        auto* control = qobject_cast<Process::ControlInlet*>(port);
        REQUIRE(control);
        control->setValue(0.5f + 0.1f * channel++);
      }
    REQUIRE(channel == 8);

    JSONReader writer;
    writer.readFrom(*original);
    auto json = readJson(writer.toByteArray());
    // A document saved before the amounts existed: the same ports without them.
    // Its ids must be the ones they have today, as cables and presets use them.
    auto& saved_inlets = json["Inlets"];
    for(auto it = saved_inlets.Begin(); it != saved_inlets.End();)
    {
      if(it->HasMember("Custom")
         && JsonValue{(*it)["Custom"]}.toString() == QStringLiteral("Pitch Env Amount (st)"))
        it = saved_inlets.Erase(it);
      else
        ++it;
    }
    REQUIRE(saved_inlets.Size() == original->inlets().size() - 8);
    int old_id = 0;
    for(auto& port : saved_inlets.GetArray())
      CHECK(port["id"].GetInt() == old_id++);
    JSONObject::Deserializer des{json};
    std::unique_ptr<Process::ProcessModel> loaded{
        deserialize_interface(processes, des, doc->context(), &doc->model())};
    REQUIRE(loaded);
    REQUIRE(loaded->inlets().size() == original->inlets().size());
    for(auto* port : original->inlets())
    {
      auto* restored = loaded->inlet(port->id());
      REQUIRE(restored);
      CHECK(restored->name() == port->name());
      if(auto* control = qobject_cast<Process::ControlInlet*>(port))
      {
        auto* restored_control = qobject_cast<Process::ControlInlet*>(restored);
        REQUIRE(restored_control);
        CHECK(restored_control->value() == control->value());
      }
    }
  });
}
