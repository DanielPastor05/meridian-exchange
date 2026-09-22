import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {execFileSync} from 'node:child_process';
const [build = 'build', configuration = 'Release'] = process.argv.slice(2);
const cache = fs.readFileSync(path.join(build, 'CMakeCache.txt'), 'utf8');
const field = name => cache.match(new RegExp('^' + name + ':[^=]*=(.*)$', 'm'))?.[1].trim() ?? null;
const compilerFile = fs.readdirSync(path.join(build, 'CMakeFiles')).map(name => path.join(build, 'CMakeFiles', name, 'CMakeCXXCompiler.cmake')).find(file => fs.existsSync(file));
if (!compilerFile) throw Error('configured compiler metadata missing');
const compiler = fs.readFileSync(compilerFile, 'utf8');
const compilerField = name => compiler.match(new RegExp('set\\(' + name + ' "([^"\\r\\n]*)"\\)'))?.[1] ?? null;
const git = args => execFileSync('git', args, {encoding: 'utf8'}).trim();
console.log(JSON.stringify({
  commit: process.env.GITHUB_SHA ?? git(['rev-parse', 'HEAD']),
  dirty: git(['status', '--porcelain']).length !== 0,
  platform: process.platform, architecture: process.arch, osRelease: os.release(),
  runnerImage: process.env.ImageOS ?? null, runnerImageVersion: process.env.ImageVersion ?? null,
  node: process.version, configuration,
  compilerId: compilerField('CMAKE_CXX_COMPILER_ID'), compilerVersion: compilerField('CMAKE_CXX_COMPILER_VERSION'),
  compilerPath: compilerField('CMAKE_CXX_COMPILER'), generator: field('CMAKE_GENERATOR'),
  flags: [field('CMAKE_CXX_FLAGS'), field('CMAKE_CXX_FLAGS_' + configuration.toUpperCase())],
  cmake: field('CMAKE_CACHE_MAJOR_VERSION') + '.' + field('CMAKE_CACHE_MINOR_VERSION') + '.' + field('CMAKE_CACHE_PATCH_VERSION'),
}, null, 2));
