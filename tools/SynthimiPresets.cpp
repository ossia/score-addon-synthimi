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
#include <tools/SynthimiPresets.hpp>

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
