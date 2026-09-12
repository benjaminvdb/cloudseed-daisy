# Demo clips

`make_demos.sh` renders one clip per Cloud Seed program into `clips/`, which is
not committed — the clips are built, not stored.

Every clip plays the same dry phrase and then that phrase through one program:

- **The same source for all ten**, so the clips compare. A strummed guitar
  chord for the tone, a snare for the transient, a piano note left ringing, with
  gaps to hear into.
- **One gain for all ten**, taken from the loudest program. A program that is
  louder than another sounds louder here too, rather than every clip being
  normalized to the same peak.
- **The preset's own dry/wet balance.** Nothing is adjusted per program.
- **The whole tail**, down to 60 dB below full scale, then a second of silence.

## Rebuilding them

```sh
./make_demos.sh
```

Needs a C++14 compiler and ffmpeg. It builds the renderer, assembles the dry
phrase, renders all ten programs, sets one common level, draws a card from each
clip's own waveform, and encodes an MP4 — then decodes what it just encoded and
lowers the level if the encoder overshot full scale.

`render_demo.cpp` is the renderer on its own: it reads a 48 kHz WAV, runs it
through one program with `cloudseed::ReverbController`, and writes the result
with its tail.

```sh
g++ -O2 -std=c++14 -ffp-contract=off -I ../src render_demo.cpp ../src/cloudseed/*.cpp -o render
./render "Dull Echoes" in.wav out.wav 14      # 14 seconds of tail
./render                                      # lists the ten program names
```

## Dry sources

All three are CC0 (public domain) from Freesound. Credit is not required by the
licence; it is here because it should be.

| File | Sound | By | Freesound |
|---|---|---|---|
| `dry/guitar-chord.mp3` | AcousticGuitar-C-Chord.wav | spitefuloctopus | [315706](https://freesound.org/s/315706/) |
| `dry/piano.mp3` | Piano G.wav | pinkyfinger | [68448](https://freesound.org/s/68448/) |
| `dry/snare.mp3` | Alexthegr81_Punchy Snare_Evolution_2.wav | alexthegr81 | [221083](https://freesound.org/s/221083/) |

These are Freesound's own preview renders, which is what the site serves without
an account.

## Putting the clips in the README

GitHub renders a video player only for an MP4 served from its attachment CDN: a
file committed here and linked by relative path becomes a plain link, and so
does a file inside a table cell or behind `[text](url)` link syntax. The player
appears only for a bare URL on a line of its own.

1. Open a new issue on the repository, or any comment box. It does not have to
   be submitted.
2. Drag `clips/<program>.mp4` into the text area and wait for the upload.
3. Copy the `https://github.com/user-attachments/assets/...` URL it inserts.
4. In the root `README.md`, put that URL on the empty line under the matching
   `<!-- player: ... -->` comment, and delete the comment.

The comments in the README name the file each slot expects.
