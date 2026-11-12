#!/usr/bin/env python3
"""Reproducible Linux ARM64 builds in Docker; no dependency on the old workspace."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
SOURCE='/sources/orbvi_driver'
VOLUMES=['-v',str(ROOT)+':'+SOURCE,'-v','orbvi-driver-build:/work']

def run(args):subprocess.run([str(x) for x in args],check=True)
def shell(image,body):run(['docker','run','--rm',*VOLUMES,image,'bash','-c','set -e; '+body])
def image(version):
    run(['docker','build','--platform','linux/arm64','-f',ROOT/'tools'/('Dockerfile.'+version),
         '-t','orbvi-driver:'+('humble' if version=='ros2' else 'noetic'),ROOT/'tools'])
def core(image_name,build,install):
    shell(image_name,f'cmake -S {SOURCE} -B /work/{build} -DCMAKE_BUILD_TYPE=Release '
          f'-DORBVI_SDK_SOURCE_DIR={SOURCE}/third_party/orbvi-4p-sdk -DCMAKE_INSTALL_PREFIX=/work/{install}; '
          f'cmake --build /work/{build} -j3; (cd /work/{build} && ctest --output-on-failure); cmake --install /work/{build}')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target',choices=['all','core','board','ros1','ros2'],default='all',nargs='?')
    target=parser.parse_args().target
    if target in ['all','core','board','ros2']:
        image('ros2')
        core('orbvi-driver:humble','core-build','driver-install')
    if target in ['all','board']:
        shell('orbvi-driver:humble',f'mkdir -p /work/board-src; '
          f'tar -C {SOURCE}/third_party/orbvi-4p-demo-ros --exclude=.build --exclude=dist -cf - . | tar -C /work/board-src -xf -; '
          'cd /work/board-src; CXX=g++ ./scripts/build-customer.sh; ./apps/test.sh; '
          f'mkdir -p {SOURCE}/backend; cp -a dist/. {SOURCE}/backend/; '
          f'g++ -O2 -std=c++17 -I{SOURCE}/include {SOURCE}/src/board_main.cpp {SOURCE}/src/board.cpp {SOURCE}/src/config.cpp '
          f'/usr/lib/aarch64-linux-gnu/libyaml-cpp.a -pthread -static-libstdc++ -static-libgcc -o {SOURCE}/tools/orbvi_board-aarch64')
    if target in ['all','ros2']:
        shell('orbvi-driver:humble','source /opt/ros/humble/setup.bash; cd /work; '
          f'colcon --log-base /work/ros2-log build --base-paths {SOURCE}/ros2 --build-base /work/ros2-build '
          '--install-base /work/unified-ros2-install --cmake-args -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/work/driver-install')
    if target in ['all','ros1']:
        archive=ROOT/'tools/ffmpeg-4.4.5.tar.xz'
        expected='f9514e0d3515aee5a271283df71636e1d1ff7274b15853bcd84e144be416ab07'
        if not archive.exists():
            temporary=archive.with_suffix('.download')
            urllib.request.urlretrieve('https://ffmpeg.org/releases/ffmpeg-4.4.5.tar.xz',temporary)
            if hashlib.sha256(temporary.read_bytes()).hexdigest()!=expected:raise RuntimeError('FFmpeg checksum mismatch')
            temporary.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=expected:raise RuntimeError('FFmpeg checksum mismatch')
        image('ros1')
        core('orbvi-driver:noetic','core-noetic-build','driver-noetic-install')
        shell('orbvi-driver:noetic','source /opt/ros/noetic/setup.bash; '
          f'cmake -S {SOURCE}/ros1/orbvi_driver_ros1 -B /work/ros1-build -DCMAKE_BUILD_TYPE=Release '
          '"-DCMAKE_PREFIX_PATH=/work/driver-noetic-install;/opt/ros/noetic" -DCMAKE_INSTALL_PREFIX=/work/unified-ros1-install; '
          'cmake --build /work/ros1-build -j3; cmake --install /work/ros1-build')
    run(['python3',ROOT/'tests/test_tunnel.py'])

if __name__=='__main__':main()
