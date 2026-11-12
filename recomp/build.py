"""Building a game: the runtime (built once, -O2, shared by every game) and the
generated C++ (-Og by default: the generated functions are large and many, and
g++'s optimiser is superlinear in a function's size), with msys2's ucrt64 g++,
and SDL2 for --window when it is there.
"""
import concurrent.futures
import os
import shutil
import subprocess
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(os.path.dirname(HERE), 'runtime')
RUNTIME_SRC = ('gb_core.cpp', 'gb_video.cpp', 'gb_sound.cpp', 'gb_trace.cpp', 'gb_main.cpp')
RT_OBJ = os.path.join(os.path.dirname(HERE), 'build', 'rt_obj')
GXX_DIR = r'C:\msys64\ucrt64\bin'
_rt_lock = threading.Lock()


def build(out, files, opt='-Og', log=print, history=False):
    env = dict(os.environ)
    if os.path.isdir(GXX_DIR):
        env['PATH'] = GXX_DIR + os.pathsep + env.get('PATH', '')
    gxx = shutil.which('g++', path=env['PATH']) or 'g++'
    sdl = os.path.isfile(os.path.join(os.path.dirname(GXX_DIR), 'include', 'SDL2', 'SDL.h'))
    flags = ['-std=c++17', opt, '-w', f'-I{RUNTIME}', f'-I{out}']
    if sdl:
        flags.append('-DGB_SDL')
    if history:
        flags.append('-DGB_HISTORY')
    objs_dir = os.path.join(out, 'obj' + ('_hist' if history else ''))
    rt_dir = RT_OBJ + ('_hist' if history else '') + ('_sdl' if sdl else '')
    os.makedirs(objs_dir, exist_ok=True)
    os.makedirs(rt_dir, exist_ok=True)
    newest_rt = max(os.path.getmtime(os.path.join(RUNTIME, n)) for n in os.listdir(RUNTIME)
                    if n.endswith(('.h', '.cpp')))

    def cc(src, obj, fl):
        r = subprocess.run([gxx, *fl, '-c', src, '-o', obj], env=env, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f'{src}:\n{r.stderr[:4000]}')
        return obj

    def rt(name):
        # the runtime does not depend on the game: built once (-O2), shared
        obj = os.path.join(rt_dir, os.path.splitext(name)[0] + '.o')
        if not (os.path.exists(obj) and os.path.getmtime(obj) > newest_rt):
            cc(os.path.join(RUNTIME, name), obj + '.tmp', [('-O2' if f == opt else f) for f in flags])
            os.replace(obj + '.tmp', obj)
        return obj
    with _rt_lock:
        rt_objs = [rt(n) for n in RUNTIME_SRC]
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        objs = rt_objs + list(ex.map(lambda f: cc(f, os.path.join(objs_dir, os.path.splitext(
            os.path.basename(f))[0] + '.o'), flags), files))
    exe = os.path.join(out, 'gb.exe')
    libs = ['-lmingw32', '-lSDL2main', '-lSDL2'] if sdl else []
    if os.name == 'nt':
        libs.append('-lshell32')         # CommandLineToArgvW: the ROM's path in UTF-16
    r = subprocess.run([gxx, *objs, '-o', exe, '-static-libgcc', '-static-libstdc++', *libs],
                       env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[:4000])
    dll = os.path.join(GXX_DIR, 'SDL2.dll')
    if sdl and os.path.isfile(dll) and not os.path.exists(os.path.join(out, 'SDL2.dll')):
        shutil.copy2(dll, out)
    log(f'  built {exe}')
    return exe
