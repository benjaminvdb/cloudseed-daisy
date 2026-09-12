#!/usr/bin/env python3
"""Exercise the actual WAV reader/writer and renderer CLI under sanitizers."""
import math
import os
import pathlib
import struct
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent


def chunk(name, data):
    return name + struct.pack('<I', len(data)) + data + bytes(len(data) % 2)


def fmt(tag=1, channels=1, bits=16, rate=48000, block=None, byte_rate=None):
    block = channels * (bits // 8) if block is None else block
    return struct.pack('<HHIIHH', tag, channels, rate,
                       rate * block if byte_rate is None else byte_rate, block, bits)


def wav(*chunks):
    body = b'WAVE' + b''.join(chunks)
    return b'RIFF' + struct.pack('<I', len(body)) + body


with tempfile.TemporaryDirectory() as directory:
    work = pathlib.Path(directory)
    # Include the translation unit so these are the renderer's own functions.
    harness = work / 'render.cpp'
    harness.write_text('''#define main RenderMain
#include "''' + str(root / 'demo/render_demo.cpp') + '''"
#undef main
int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "read") {
    Audio audio;
    if (!ReadWav(argv[2], audio)) return 1;
    for (size_t i = 0; i < audio.left.size(); ++i)
      std::printf("%.9g %.9g\\n", audio.left[i], audio.right[i]);
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "write") {
    Audio audio; audio.left = {.5f}; audio.right = {-.25f};
    return WriteWav(argv[2], audio) ? 0 : 1;
  }
  return RenderMain(argc - 1, argv + 1);
}
''')
    binary = work / 'render'
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++14', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-ffp-contract=off',
                    '-fsanitize=address,undefined,float-cast-overflow',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                    '-I', str(root / 'src'), str(harness),
                    *map(str, sorted((root / 'src/cloudseed').glob('*.cpp'))),
                    '-o', str(binary)], check=True)

    def run(*args, success):
        result = subprocess.run([str(binary), *map(str, args)],
                                capture_output=True, text=True)
        assert result.returncode == (0 if success else 1) or (
            not success and result.returncode == 2), (args, result)
        assert 'runtime error:' not in result.stderr and 'Sanitizer' not in result.stderr, result
        return result.stdout

    good_fmt = chunk(b'fmt ', fmt())
    pcm = chunk(b'data', struct.pack('<hhh', -32768, 0, 32767))
    good = wav(good_fmt, pcm)
    fixtures = [
        (good, [-1., -1., 0., 0., 32767 / 32768, 32767 / 32768]),
        (wav(pcm, chunk(b'JUNK', b'x'), good_fmt),
         [-1., -1., 0., 0., 32767 / 32768, 32767 / 32768]),
        (wav(chunk(b'fmt ', fmt(bits=24)),
             chunk(b'data', b'\0\0\x80\xff\xff\xff\xff\xff\x7f')),
         [-1., -1., -1 / 8388608, -1 / 8388608, 8388607 / 8388608, 8388607 / 8388608]),
        (wav(chunk(b'fmt ', fmt(tag=3, channels=2, bits=32)),
             chunk(b'data', struct.pack('<ff', .5, -.25))), [.5, -.25]),
        (wav(good_fmt, chunk(b'data', b'')), []),
    ]
    path = work / 'input.wav'
    for data, expected in fixtures:
        path.write_bytes(data)
        actual = list(map(float, run('read', path, success=True).split()))
        assert len(actual) == len(expected)
        assert all(math.isclose(a, b, abs_tol=1e-8) for a, b in zip(actual, expected))

    malformed = [b'', good[:11], good[:-1],
                 wav(chunk(b'fmt ', b'x'), pcm), wav(pcm), wav(good_fmt),
                 wav(good_fmt, pcm, pcm), wav(good_fmt, good_fmt, pcm),
                 wav(good_fmt, chunk(b'data', b'x')),
                 wav(good_fmt, b'data\xff\xff\xff\xff'),
                 b'RIFF\xff\xff\xff\xff' + good[8:],
                 b'RIFF\x04\0\0\0' + good[8:],
                 wav(chunk(b'JUNK', b'x'))[:-1]]
    for fields in ({'bits': 0}, {'bits': 8}, {'bits': 32}, {'tag': 6},
                   {'channels': 0}, {'channels': 3}, {'block': 0},
                   {'block': 4}, {'byte_rate': 0}, {'rate': 16000}):
        malformed.append(wav(chunk(b'fmt ', fmt(**fields)), pcm))
    for value in (math.nan, math.inf):
        malformed.append(wav(chunk(b'fmt ', fmt(tag=3, bits=32)),
                             chunk(b'data', struct.pack('<f', value))))
    for data in malformed:
        path.write_bytes(data)
        run('read', path, success=False)

    output = work / 'output.wav'
    run('write', output, success=True)
    assert list(map(float, run('read', output, success=True).split())) == [.5, -.25]
    path.write_bytes(good)
    for tail in ('nan', 'inf', '-1', '', 'no', '1oops', '1e999', '1000000000'):
        run('render', 'Small Room', path, output, tail, success=False)
    run('render', 'Small Room', path, output, '0', success=True)
    assert output.stat().st_size == 44 + 3 * 8
    if pathlib.Path('/dev/full').exists():
        run('write', '/dev/full', success=False)
        run('render', 'Small Room', path, '/dev/full', '0', success=False)
    print(f'WAV reader/writer and CLI: {len(fixtures)} valid, '
          f'{len(malformed)} malformed fixtures; output and argument checks passed')
