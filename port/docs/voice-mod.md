# Voice mod pack (cutscene voices)

A mod pack can replace the voices of the cutscenes with a dub made for the Steam release (RE4 UHD), for example the
Latin American Spanish dub by IG Studios (wish of DevJunior, 26.11.2026). The files stay yours: nothing of them goes
into the repository, and the game's disc files are not changed.

## Files

Two files of the Steam release's dub, from its `BIO4/snd/` folder:

| File | What |
|---|---|
| `bio4evt.xwb` | the voices (XACT wave bank, PCM or MS ADPCM) |
| `bio4evt.xsb` | which voice belongs to which scene (XACT sound bank) |

Put both in a folder `voice` of a mod pack, e.g. `mods/Doblaje Latino/voice/bio4evt.xwb`, and add the pack through the
app's menu (Mods). A PC mod's folder added as it is works too (`BIO4/snd/bio4evt.*` is found); its other files are
left out, but copying the whole PC mod takes much more space.

## How it plays

- When the game starts a scene's voice, the dub's line for that scene plays instead; the game's own voice runs on
  silently, so the scene keeps its timing.
- The line follows the game's volume, its fades and the app's music volume.
- A line longer than the game's goes on while the scene lasts; skipping the scene stops it.
- Voices outside the cutscenes (Leon's and the enemies' shouts) stay the game's for now.

The log (`voice mod:` lines) tells for each scene which line plays and how long it is.
