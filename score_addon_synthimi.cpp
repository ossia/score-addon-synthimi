#include "score_addon_synthimi.hpp"

#include <Avnd/Factories.hpp>
#include <Synthimi/Synthimi.hpp>
#include <score/plugins/FactorySetup.hpp>
#include <score_plugin_engine.hpp>

/**
 * This file instantiates the classes that are provided by this plug-in.
 */
score_addon_synthimi::score_addon_synthimi() = default;
score_addon_synthimi::~score_addon_synthimi() = default;

std::vector<std::unique_ptr<score::InterfaceBase>> score_addon_synthimi::factories(
    const score::ApplicationContext& ctx,
    const score::InterfaceKey& key) const
{
  std::vector<std::unique_ptr<score::InterfaceBase>> fx;
  Avnd::instantiate_fx<Synthimi::Synthimi>(fx, ctx, key);
  return fx;
}

std::vector<score::PluginKey> score_addon_synthimi::required() const
{
  return {score_plugin_engine::static_key()};
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_synthimi)
