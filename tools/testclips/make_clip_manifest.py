"""Build the real-footage clip manifest (manifest.json) from the clip library's README.

Usage:  python -I make_clip_manifest.py [--root D:\\testclips] [--out <root>\\manifest.json] [--probe]

The README (D:\\testclips\\README.md) lists every clip with its rate, cadence and the frame
numbers / timestamps of its planned events. This script parses its tables into a typed,
machine-readable manifest that the clip-driven tests (RealFootageSuiteTests.cpp) read:
nothing about a clip (resolution, rate, events) is hard-coded in the tests.

Scene-cut ground truth for the 3-minute trims comes from the library's own ffmpeg scene
statistics (_scripts\\<src>_cuts.txt, seconds on the source clock), mapped onto each clip's
clock with the trim start and the clip's retime.

--probe checks each clip with ffprobe (packet count, size) and records the measured values.
Only the standard library is used.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from fractions import Fraction

RATES = {
    '23.976': (24000, 1001), '29.97': (30000, 1001), '59.94': (60000, 1001), '119.88': (120000, 1001),
}


def rate_of(text):
    """'23.976' -> (24000, 1001); '25' -> (25, 1)."""
    text = text.strip()
    if text in RATES:
        return RATES[text]
    f = Fraction(text).limit_denominator(1001)
    return f.numerator, f.denominator


def parse_tables(readme):
    """Yields (section, cells) for every table row of the README (header/separator skipped)."""
    section = ''
    for line in readme.splitlines():
        if line.startswith('## '):
            section = line[3:].strip()
            continue
        if not line.startswith('|') or line.startswith('|---') or line.startswith('| File') or line.startswith('| Folder') \
                or line.startswith('| Env var'):
            continue
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) >= 7:
            yield section, cells


def section_dir(section):
    """'variants\\ - 3-minute trims' -> 'variants'; 'timing\\rates - ...' -> 'timing'."""
    head = section.split(' ')[0].rstrip('\\')
    return head.split('\\')[0]


def strip_md(text):
    return text.replace('`', '').replace('**', '')


def cut_list(scripts, src):
    path = os.path.join(scripts, '%s_cuts.txt' % src)
    if not os.path.exists(path):
        return []
    out = []
    with open(path, encoding='utf-8-sig') as f:
        for line in f:
            line = line.strip().replace('pts_time:', '')
            if line:
                out.append(float(line))
    return out


# Trim start and film-frame clock of each 3-minute variant: (source, trim start s, source fps,
# clip seconds per source frame). Cut times map as t_clip = (t_src - start) * fps_src * spf.
def variant_clock(name):
    if 'longtake' in name or 'credits' in name:
        return None   # cut-free shots from other source windows
    if name.startswith('tos_'):
        src, start, fps = 'tos', 520.0, 24.0
        if '25p' in name or '_22_' in name:
            spf = 1.0 / 25.0
        elif '23976' in name or '5994' in name:
            spf = 1001.0 / 24000.0
        else:
            spf = 1.0 / 24.0
        return src, start, fps, spf
    if name.startswith('cosmos_'):
        return 'cos', 300.0, 24000.0 / 1001.0, 1001.0 / 24000.0
    if name.startswith('meridian_'):
        return 'mer', 90.0, 60000.0 / 1001.0, 1001.0 / 60000.0
    return None


def ints(text):
    return [int(x) for x in re.findall(r'\d+', text)]


def parse_events(name, events_text, rate):
    """Typed events from a README 'Use / events' cell (after the first <br>)."""
    num, den = rate
    period = den / num
    ev = []
    t = strip_md(events_text)
    # dropped frames (frames removed, timestamps left as gaps)
    m = re.search(r'clip frame n removed[^:]*:(.*?)(<br>|$)', t)
    if m:
        removed = [int(x) for x in re.findall(r'(\d+) \(src', m.group(1))]
        for i, n in enumerate(sorted(removed)):
            ev.append({'type': 'drop', 'original_frame': n, 'frame': n + 1 - (i + 1), 'missed': 1,
                       't': round(n * period, 6)})
    # duplicated frames
    for m in re.finditer(r'source frame (\d+) \(src \d+\) shown (\d+) times: output frames (\d+)\.\.(\d+)', t):
        first, last = int(m.group(3)), int(m.group(4))
        ev.append({'type': 'duplicate', 'source_frame': int(m.group(1)), 'first': first, 'last': last,
                   'repeats': list(range(first + 1, last + 1))})
    m = re.search(r'black frames: output frames (\d+)\.\.(\d+)', t)
    if m:
        ev.append({'type': 'black', 'first': int(m.group(1)), 'last': int(m.group(2))})
    m = re.search(r'frozen: output frames (\d+)\.\.(\d+)', t)
    if m:
        ev.append({'type': 'frozen', 'first': int(m.group(1)), 'last': int(m.group(2))})
    m = re.search(r'no frames for clip frames (\d+)\.\.(\d+).*?gap length (\d+) periods = ([\d.]+) s', t)
    if m:
        first, last = int(m.group(1)), int(m.group(2))
        ev.append({'type': 'pts_gap', 'frame': first, 'missing_first': first, 'missing_last': last,
                   'periods': int(m.group(3)), 'seconds': float(m.group(4))})
    m = re.search(r'rate change at clip frame (\d+), t=([\d.]+) s', t)
    if m:
        ev.append({'type': 'rate_change', 'frame': int(m.group(1)), 't': float(m.group(2)),
                   'rate_num': 60000, 'rate_den': 1001, 'content_after': 'video'})
    m = re.search(r'deterministic jitter of -?(\d+)\.\.\+?(\d+) ms', t)
    if m or 'jitter' in name:
        ev.append({'type': 'jitter', 'ms': 2.0})
    # cadence segments, film / native 30p (cad_32_30p_32_30p)
    for m in re.finditer(r'segment (\d+) (FILM 3:2|NATIVE 30p) \(.*?\): output frames (\d+)\.\.(\d+)', t):
        ev.append({'type': 'segment', 'index': int(m.group(1)), 'first': int(m.group(3)), 'last': int(m.group(4)),
                   'kind': 'film32' if m.group(2).startswith('FILM') else 'video30'})
    # phase-jump segments (cad_32_phase_jumps)
    for m in re.finditer(r'segment (\d+): output frames (\d+)\.\.(\d+) \([^)]*\), source start (\d+)s, pattern ([\d,]+)(.*?)(<br>|$)', t):
        cont = 'CONTINUES' in m.group(6)
        ev.append({'type': 'segment', 'index': int(m.group(1)), 'first': int(m.group(2)), 'last': int(m.group(3)),
                   'kind': 'film32', 'pattern': [int(x) for x in m.group(5).strip(',').split(',')][:2],
                   'source_start': int(m.group(4)), 'cut': not cont and int(m.group(1)) > 0})
    m = re.search(r'phase-break boundaries[^:]*: ([\d, ]+);', t)
    if m:
        for f in ints(m.group(1)):
            ev.append({'type': 'phase_break', 'frame': f})
    # 2:2 slips (cad_22_50p_phase_breaks)
    for m in re.finditer(r'film frame (\d+) \(output frames (\d+)\.\.(\d+), ([\d.]+) s\) shown (\d+) times instead of (\d+)', t):
        ev.append({'type': 'cadence_slip', 'film_frame': int(m.group(1)), 'first': int(m.group(2)), 'last': int(m.group(3)),
                   'shown': int(m.group(5)), 'normal': int(m.group(6)), 't': float(m.group(4))})
    # fades
    m = re.search(r'fade-in from black t=([\d.]+)\.\.([\d.]+) s, fade-out to black t=([\d.]+)\.\.([\d.]+) s', t)
    if m:
        ev.append({'type': 'fade_in', 't0': float(m.group(1)), 't1': float(m.group(2))})
        ev.append({'type': 'fade_out', 't0': float(m.group(3)), 't1': float(m.group(4))})
    m = re.search(r'pan at (\d+) px/s', t)
    if m:
        ev.append({'type': 'pan', 'pixels_per_second': int(m.group(1))})
    return ev


def classify(name, cadence, rate, interlaced):
    """Expected content class and picture (film) rate from the README's cadence text."""
    c = cadence.lower()
    num, den = rate
    info = {}
    if name.startswith('meridian') or 'mer' == name[:3]:
        info['content'] = 'video'
        info['picture_rate'] = [num, den]
        if num * 1.0 / den < 31:
            # Owner's film / video split (DejudderPolicy::IsFilmLike): 24/25/30p are film-like
            # by rate; the README calls Meridian 29.97p video (a negative for film by content).
            info['content_rate_class'] = 'film-like (owner split: 30p)'
    else:
        info['content'] = 'film'
    if 'pan_' in name:
        info['content'] = 'synthetic-pan'
    if 'telecine' in c or 'pattern=23' in c:
        info['cadence'] = {'kind': 'field_telecine_23'}
        info['picture_rate'] = [24000, 1001]
    elif '2:2 pulldown in 50i' in c:
        info['cadence'] = {'kind': 'psf_22'}
        info['picture_rate'] = [25, 1]
    elif '2:3' in c and 'frame-repeat' in c or 'exact 2:3' in c:
        info['cadence'] = {'kind': 'pulldown', 'holds': [2, 3]}
        info['picture_rate'] = [num * 2, den * 5]
    elif 'exact 2:2 frame repeat' in c:
        info['cadence'] = {'kind': 'pulldown', 'holds': [2]}
        info['picture_rate'] = [num, den * 2]
    elif 'source frame k shown c(k)' in c:
        info['cadence'] = {'kind': 'rate_conversion', 'from_rate': [24, 1]}
        info['picture_rate'] = [24, 1]
    elif 'every second frame' in c or 'native 29.97p video' in c:
        info['cadence'] = {'kind': 'native'}
    else:
        info['cadence'] = {'kind': 'native'}
        info.setdefault('picture_rate', [num, den])
    if interlaced:
        info['interlaced'] = True
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=r'D:\testclips')
    ap.add_argument('--out', default=None)
    ap.add_argument('--ffmpeg', default=r'D:\tools\ffmpeg\bin\ffmpeg.exe')
    ap.add_argument('--probe', action='store_true')
    a = ap.parse_args()
    readme_path = os.path.join(a.root, 'README.md')
    with open(readme_path, encoding='utf-8') as f:
        readme = f.read()
    scripts = os.path.join(a.root, '_scripts')
    cuts = {src: cut_list(scripts, src) for src in ('tos', 'cos', 'mer')}
    clips = []
    for section, cells in parse_tables(readme):
        base = section_dir(section)
        if base not in ('variants', 'film', 'timing'):
            continue
        rel = strip_md(cells[0])
        path = os.path.join(base, rel.replace('/', '\\'))
        name = os.path.splitext(os.path.basename(rel))[0]
        rate_cell = strip_md(cells[1])
        interlaced = 'interlaced' in rate_cell
        vfr = 'VFR' in rate_cell
        m = re.match(r'\s*([\d.]+)', rate_cell)
        rate = rate_of(m.group(1))
        if 'header F' in rate_cell:
            # film\ y4m: the header carries a whole-number label; the frames are unchanged.
            rate = rate_of(m.group(1))
        frames = int(re.sub(r'[^\d]', '', cells[2]) or 0)
        seconds = float(re.match(r'([\d.]+)', cells[3]).group(1))
        cadence = strip_md(cells[5])
        use = strip_md(cells[6])
        parts = use.split('<br>')
        clip = {
            'id': name, 'path': path, 'group': base if base != 'timing' else 'timing/' + rel.split('/')[0],
            'container': os.path.splitext(rel)[1].lstrip('.'),
            'rate_num': rate[0], 'rate_den': rate[1], 'frames': frames, 'seconds': seconds,
            'interlaced': interlaced, 'vfr': vfr, 'cadence_text': cadence, 'use': parts[0].strip(),
        }
        clip.update(classify(name, cadence, rate, interlaced))
        if 'rate change' in parts[0] or 'ratechange' in name:
            clip['content'] = 'film-then-video'
        clip['events'] = parse_events(name, '<br>'.join(parts[1:]) + ' ' + cadence + ' ' + parts[0], rate)
        if name.startswith('cad_32_30p'):
            clip['content'] = 'film-and-video30'
            clip['cadence'] = {'kind': 'segments'}
        if name.startswith('cad_mixed'):
            clip['content'] = 'film-with-30p-overlay'
            clip['cadence'] = {'kind': 'mixed', 'base_holds': [2, 3], 'overlay_holds': [2]}
        if name.startswith('cad_22'):
            clip['cadence'] = {'kind': 'pulldown', 'holds': [2]}
            clip['picture_rate'] = [25, 1]
        if name.startswith('cad_32_phase'):
            clip['cadence'] = {'kind': 'pulldown', 'holds': [2, 3]}
            clip['picture_rate'] = [24000, 1001]
        if 'cosmos' in name:
            clip['source'] = 'cosmos'
            clip['note'] = 'CG animated feature at ones (every frame unique): film, not drawn animation'
        elif name.startswith('tos') or name.startswith('dis_') or name.startswith('cad_') or name.startswith('rate_') \
                or name.startswith('drift') or name.startswith('pan_'):
            clip['source'] = 'tos'
        elif name.startswith('meridian'):
            clip['source'] = 'meridian'
        if 'dark' in name:
            clip['dark'] = True
        # Cut-free shots and scene-cut ground truth.
        if 'longtake' in name:
            clip['cut_free'] = True
            clip['long_take'] = True
        if 'credits_scroll' in name or name.startswith('pan_') or name.startswith('rate_') or name.startswith('dis_') \
                or name.startswith('drift'):
            if not name.startswith('dis_ratechange'):
                clip['cut_free'] = True
        vc = variant_clock(name) if base == 'variants' else None
        if vc:
            src, start, fps, spf = vc
            clip_cuts = []
            for c in cuts[src]:
                tc = (c - start) * fps * spf
                if 0.5 < tc < seconds - 0.5:
                    clip_cuts.append(round(tc, 4))
            clip['cuts_s'] = clip_cuts
            clip['cuts_source'] = '_scripts\\%s_cuts.txt (ffmpeg scene score), trim %g s' % (src, start)
        if a.probe:
            fp = os.path.join(os.path.dirname(a.ffmpeg), 'ffprobe.exe')
            full = os.path.join(a.root, path)
            r = subprocess.run([fp, '-v', 'error', '-select_streams', 'v:0', '-count_packets', '-show_entries',
                                'stream=width,height,nb_read_packets', '-of', 'json', full], capture_output=True, text=True)
            s = json.loads(r.stdout)['streams'][0]
            clip['width'], clip['height'] = s['width'], s['height']
            clip['probed_frames'] = int(s['nb_read_packets'])
        clips.append(clip)
    manifest = {
        'version': 1,
        'generator': 'tools/testclips/make_clip_manifest.py',
        'readme': readme_path,
        'root': a.root,
        'ffmpeg': a.ffmpeg,
        'clips': clips,
    }
    out = a.out or os.path.join(a.root, 'manifest.json')
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=1)
    print('%d clips -> %s' % (len(clips), out))
    for c in clips:
        print('  %-44s %6d frames %8.3f Hz %-22s %2d events%s' % (
            c['id'], c['frames'], c['rate_num'] / c['rate_den'], c['cadence']['kind'], len(c['events']),
            (' %d cuts' % len(c['cuts_s'])) if 'cuts_s' in c else ''))


if __name__ == '__main__':
    sys.exit(main())
