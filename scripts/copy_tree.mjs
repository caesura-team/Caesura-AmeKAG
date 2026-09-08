import { chmodSync, copyFileSync, cpSync, lstatSync, mkdirSync,
         readdirSync, realpathSync, unlinkSync } from 'node:fs'
import { basename, dirname, isAbsolute, join, relative, resolve, sep } from 'node:path'

function assertDestinationOutside(source, destination) {
  const below = relative(source, destination)
  if (!below || (!isAbsolute(below) && below !== '..' && !below.startsWith('..' + sep))) {
    throw new Error('Cannot copy a directory into itself: ' + source + ' -> ' + destination)
  }
}

function canonicalDestination(destination) {
  let ancestor = destination
  const suffix = []
  for (;;) {
    try {
      return resolve(realpathSync.native(ancestor), ...suffix)
    } catch (error) {
      if (error.code !== 'ENOENT') throw error
      const parent = dirname(ancestor)
      if (parent === ancestor) throw error
      suffix.unshift(basename(ancestor))
      ancestor = parent
    }
  }
}

// Node 22's native recursive cpSync/overwrite paths can reinterpret UTF-8 as
// the Windows ANSI code page. Use UTF-8-aware fs primitives for directory and
// regular-file copies, including overwrites; an always-true cpSync filter alone
// still enters the broken native overwrite path for existing regular files.
export function copyDirectorySync(source, destination) {
  const src = resolve(source), dest = resolve(destination)
  assertDestinationOutside(src, dest)
  const sourceInfo = lstatSync(src)
  if (!sourceInfo.isDirectory()) throw new Error('Copy source is not a directory: ' + src)
  const destinationInfo = lstatSync(dest, { throwIfNoEntry: false })
  if (destinationInfo && !destinationInfo.isDirectory()) {
    throw new Error('Copy destination is not a directory: ' + dest)
  }
  // A nonexistent destination can still have a parent junction/symlink that
  // points into the source. Resolve its nearest existing ancestor before any
  // mkdir/copy, then append only the missing suffix to check the actual target.
  assertDestinationOutside(realpathSync.native(src), canonicalDestination(dest))
  mkdirSync(dest, { recursive: true })
  for (const entry of readdirSync(src, { withFileTypes: true })) {
    const from = join(src, entry.name), to = join(dest, entry.name)
    if (entry.isDirectory()) {
      copyDirectorySync(from, to)
    } else if (entry.isSymbolicLink()) {
      // Preserve cpSync's existing symlink policy. Its filtered symlink branch
      // uses the JS/readlink/symlink implementation, not native directory copy.
      cpSync(from, to, { filter: () => true })
    } else if (entry.isFile()) {
      // Match force-overwrite semantics: replace the destination entry instead
      // of writing through a symlink/hardlink or into a readonly existing file.
      const previous = lstatSync(to, { throwIfNoEntry: false })
      if (previous) {
        if (!previous.isFile() && !previous.isSymbolicLink()) {
          throw new Error('Copy destination is not a file: ' + to)
        }
        unlinkSync(to)
      }
      copyFileSync(from, to)
      chmodSync(to, lstatSync(from).mode)
    } else {
      throw new Error('Unsupported file type in package: ' + from)
    }
  }
  if (!destinationInfo) chmodSync(dest, sourceInfo.mode)
}
