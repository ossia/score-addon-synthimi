#pragma once
#include <score/application/ApplicationContext.hpp>
#include <score/plugins/Interface.hpp>
#include <score/plugins/qt_interfaces/FactoryInterface_QtInterface.hpp>
#include <score/plugins/qt_interfaces/PluginRequirements_QtInterface.hpp>

#include <verdigris>

class score_addon_synthimi final
    : public score::FactoryInterface_QtInterface
    , public score::Plugin_QtInterface
{
  SCORE_PLUGIN_METADATA(1, "59d3b321-2f74-4c13-ad1c-72af76c6bb1d")
public:
  score_addon_synthimi();
  ~score_addon_synthimi() override;

private:
  std::vector<std::unique_ptr<score::InterfaceBase>> factories(
      const score::ApplicationContext&,
      const score::InterfaceKey& factoryName) const override;

  std::vector<score::PluginKey> required() const override;
};
