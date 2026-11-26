#pragma once
// CubeEvil4 Studio's converter (Mods, 19.11.2026; DevJunior's choice: the UHD skins first, a mod picked with the file
// picker, the result a .zip to import in CubeEvil4): character skins of the PC release (RE4 UHD, Steam 2014:
// em/plXX.udas, little-endian, float vertices, textures in BIO4/imagePackHD) become archives the game reads from the
// GameCube discs (em/plXX.drs, big-endian, GX display lists, textures in the archive). A port of
// port/tools/uhd_skin_convert.py and port/tools/re4lfs.py; the same output for the same input, except the textures'
// scaling (Pillow's bicubic filter, here a close one) and the painting of a missing face from the disc's (the PC
// tool's bake_texture with scipy): here, as there without scipy, the disc's colour image.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace re4::studio::convert {

using Bytes = std::vector<std::uint8_t>;

// An .lfs file of the PC release (RDLX, XMemCompress LZX chunks) unpacked; throws on a broken one.
Bytes UnpackLfs(const Bytes &data);
bool IsLfs(const Bytes &data);

// The mod's BIO4/imagePackHD/*.pack files (unpacked): textures by the palette's reference (pack id, index).
class Packs {
public:
  // a .pack file's path (its header is read now, its textures when asked)
  bool add(const std::string &path);
  std::size_t size() const { return packs_.size(); }
  struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4
  };
  bool has(std::uint32_t ref) const { return find(ref) != nullptr; }
  // the DDS image `index` of the pack `ref` as RGBA; false when missing or of a kind this does not read
  bool image(std::uint32_t ref, std::uint32_t index, Image &out, std::string *why) const;

private:
  struct Pack {
    std::uint32_t id;
    std::string path;
    std::vector<std::uint32_t> offsets;
    std::uint64_t fileSize;
  };
  const Pack *find(std::uint32_t ref) const;
  std::vector<Pack> packs_;
};

struct Options {
  int tex = 128;        // largest texture side
  int texColour = 0;    // of the colour (CMPR) textures, 0: tex
  int texOther = 0;     // of the bump, specular and alpha ones, 0: tex
  int palette = 2048;   // most skinning matrices per model
  // parts that may lie elsewhere in the PC skeleton than on the disc (same id and parent); allFree: any such
  bool allFree = true;
  std::set<int> bonesFree;
  // the skeletons differ (more or other parts): draw the main model with the disc's skeleton (Separate Ways' Ada)
  bool rebind = true;
  // GTA skins (ConvertDff): Leon's own head, face and hair stay (a skin without a head of its own); off: folded away
  bool keepHead = false;
  // GTA skins: the weights from Leon's own skin, the nearest of his vertices (0: the skin's own, mapped by bone)
  double transfer = 1.0;
  // GTA skins: the skin once more just under its surface, so that gaps at the joints show its own colours
  bool innerLayer = true;
  // GTA skins and RE4 Remake mods: at most this many triangles of the skin (0: as many as one model holds, 21845,
  // half with the inner layer); a denser skin is reduced
  std::size_t maxSkinTris = 0;
  // GTA skins and RE4 Remake mods: how much the skin is made as thin as Leon round each bone (1 all, 0 its own build)
  double fitBody = 0.0;
  // with fitBody: another converted skin (its archive) whose build round each bone this one takes instead of Leon's
  std::vector<std::uint8_t> fitRef;
  // RE4 Remake mods: the trunk sized by the thighs to the head joint (as the GTA skins by the hips to the head) instead
  // of the thighs to the shoulders
  bool trunkToHead = false;
  // the PC HUD (ConvertCoreHud): a picture this close to the disc's (mean difference per pixel, 0..255) is not the
  // mod's and stays the disc's
  double hudSame = 14.0;
};

// The disc's archive (`gcName` its file name: pl00.drs ...) with the PC mod's (unpacked .udas) converted into it.
// `log` gets the PC tool's report, a line per entry; false and `why` when it cannot be done.
bool ConvertSkin(const Bytes &gc, const std::string &gcName, const Bytes &pc, const Packs &packs, const Options &o,
                 Bytes &out, std::string &log, std::string *why);

// A GTA San Andreas ped skin (.dff, its textures in the .txd: may be empty) put on Leon's archive of the disc (`gc`,
// pl00 / pl08 / pl09 / pl10): the mesh moved onto his skeleton, his own head, face, hair, outfit and hands folded
// away (studio_dff.inc). `log` gets the report; false and `why` when it cannot be done.
bool ConvertDff(const Bytes &gc, const std::string &gcName, const Bytes &dff, const Bytes &txd, const Options &o,
                Bytes &out, std::string &log, std::string *why);

// An XNALara model (the classic binary generic_item.mesh; studio_fbx.inc) put on the character's archive the same way,
// `files` its pictures by their lower-case file name.
bool ConvertXps(const Bytes &gc, const std::string &gcName, const Bytes &mesh, const std::map<std::string, Bytes> &files,
                const Options &o, Bytes &out, std::string &log, std::string *why);

// A rigged model as a binary .fbx (7.x; studio_fbx.inc) put on the character's archive as a GTA skin: its bones by
// their names (Blender, Mixamo, Biped, Unreal), `files` its pictures (.png, .tga) by their lower-case file name.
bool ConvertFbx(const Bytes &gc, const std::string &gcName, const Bytes &fbx, const std::map<std::string, Bytes> &files,
                const Options &o, Bytes &out, std::string &log, std::string *why);

// An RE4 Remake (PC, RE Engine) character mod put on Leon's archive the same way (studio_remesh.inc): `files` are its
// files by their path below natives/STM (lower case, '/'), a colour variant's laid over the base folder's.
bool ConvertReMod(const Bytes &gc, const std::string &gcName, const std::map<std::string, Bytes> &files,
                  const Options &o, Bytes &out, std::string &log, std::string *why);

// An RE4 Remake weapon mod (its meshes under natives/STM/_Chainsaw/Character/wp/wpNN/`wp`, "" the first) put into the
// disc's weapon archive (`gc`, em/wepNN.drs): every model of the weapon anew, as long as the disc's plain one with the
// muzzles on each other, each piece rigid on the disc bone nearest to it, the textures behind the weapon's palette;
// reduced until it fits the weapon's place (0x70000 bytes).
bool ConvertReWeapon(const Bytes &gc, const std::string &gcName, const std::map<std::string, Bytes> &files,
                     const std::string &wp, const Options &o, Bytes &out, std::string &log, std::string *why);

// The PC release's HUD (a texture pack 07000000.pack, the Remake HUD mods) into the disc's core (`gc`, etc/core.das):
// the cockpit's textures (entry 25) of the same id as the PC core's (`pc`, etc/core.udas) from the pack, at a few
// times the disc's size while the core fits the place of a larger core (3 MB)
bool ConvertCoreHud(const Bytes &gc, const Bytes &pc, const Packs &packs, const Options &o, Bytes &out,
                    std::string &log, std::string *why);

// The disc's weapon archive an RE4 Remake weapon (its folder name, wp4000 ...) replaces; "" none known
const char *ReWeaponTarget(const std::string &wp);

}  // namespace re4::studio::convert
