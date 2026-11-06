// The root filesystem as a wasm module: the read-only layer embedded in the
// module and the API to read it. Built for wasip1 as a reactor; the kernel
// calls _initialize, then these exports, where a negative result is -errno:
//
//	fs_scratch() -> address              a buffer of scratchSize bytes for names
//	fs_lookup(dir, size) -> inode        the entry of dir named by size bytes of the scratch
//	fs_mode(inode) -> mode               st_mode
//	fs_size(inode) -> size               st_size
//	fs_extent(inode, offset) -> extent   the contiguous bytes at offset of a file or a
//	                                     symlink's target; an empty extent at the end
//	fs_dirent_name(dir, index) -> extent the name of a directory's entry; empty past the end
//	fs_dirent_inode(dir, index) -> inode the inode of a directory's entry
//
// An extent is address<<32 | size in the module's memory, read-only. The
// root directory is inode 0.
//
// The layer is layer.bin, written by pack.py. All little-endian u32, offsets
// from the layer's start:
//
//	header  magic "PZFS", version 1, size of the layer, number of inodes, self
//	        inode, prefix offset, prefix size, suffix offset, suffix size, 0
//	inode   mode, offset, size: bytes of a file or a symlink's target; a
//	        directory's dirents and their number
//	dirent  name offset, name size, inode; sorted by name
//
// The module's own file is in the layer without containing itself: the file
// is the prefix, the bytes before the layer in the module, then the layer,
// then the suffix, the bytes after it. Prefix and suffix are stored in the
// layer.
package main

import (
	_ "embed"
	"encoding/binary"
	"strings"
	"unsafe"
)

//go:embed layer.bin
var layer string

const (
	magic       = "PZFS"
	headerSize  = 40
	inodeSize   = 12
	direntSize  = 12
	scratchSize = 256

	enoent       = 2
	ebadf        = 9
	enotdir      = 20
	eisdir       = 21
	einval       = 22
	enametoolong = 36

	sIfmt  = 0o170000
	sIfdir = 0o040000
)

var scratch [scratchSize]byte

func init() {
	if layer[:4] != magic {
		panic("layer.bin is not a layer")
	}
}

type Inode struct {
	mode   uint32
	offset uint32
	size   uint32
}

func word(at uint32) uint32 {
	return binary.LittleEndian.Uint32([]byte(layer[at : at+4]))
}

func extent(part string) int64 {
	if len(part) == 0 {
		return 0
	}

	return int64(uintptr(unsafe.Pointer(unsafe.StringData(part))))<<32 | int64(len(part))
}

func inode(index uint32) (Inode, int64) {
	if index >= word(12) {
		return Inode{}, -ebadf
	}

	at := headerSize + inodeSize*index

	return Inode{mode: word(at), offset: word(at + 4), size: word(at + 8)}, 0
}

func directory(index uint32) (Inode, int64) {
	dir, errno := inode(index)

	if errno != 0 {
		return dir, errno
	}

	if dir.mode&sIfmt != sIfdir {
		return dir, -enotdir
	}

	return dir, 0
}

func direntName(at uint32) string {
	start := word(at)

	return layer[start : start+word(at+4)]
}

//go:wasmexport fs_scratch
func fsScratch() uint32 {
	return uint32(uintptr(unsafe.Pointer(&scratch[0])))
}

//go:wasmexport fs_lookup
func fsLookup(index uint32, size uint32) int32 {
	dir, errno := directory(index)

	if errno != 0 {
		return int32(errno)
	}

	if size > scratchSize {
		return -enametoolong
	}

	name := string(scratch[:size])
	low, high := uint32(0), dir.size

	for low < high {
		middle := low + (high-low)/2
		at := dir.offset + direntSize*middle

		switch strings.Compare(direntName(at), name) {
		case 0:
			return int32(word(at + 8))
		case -1:
			low = middle + 1
		default:
			high = middle
		}
	}

	return -enoent
}

//go:wasmexport fs_mode
func fsMode(index uint32) int32 {
	file, errno := inode(index)

	if errno != 0 {
		return int32(errno)
	}

	return int32(file.mode)
}

//go:wasmexport fs_size
func fsSize(index uint32) int64 {
	file, errno := inode(index)

	if errno != 0 {
		return errno
	}

	if index == word(16) {
		return int64(word(24)) + int64(len(layer)) + int64(word(32))
	}

	return int64(file.size)
}

//go:wasmexport fs_extent
func fsExtent(index uint32, offset int64) int64 {
	file, errno := inode(index)

	if errno != 0 {
		return errno
	}

	if file.mode&sIfmt == sIfdir {
		return -eisdir
	}

	if index == word(16) {
		return selfExtent(offset)
	}

	if offset < 0 || offset > int64(file.size) {
		return -einval
	}

	return extent(layer[file.offset+uint32(offset) : file.offset+file.size])
}

// The module's own file: the prefix, the layer itself, the suffix.
func selfExtent(offset int64) int64 {
	prefix := layer[word(20) : word(20)+word(24)]
	suffix := layer[word(28) : word(28)+word(32)]

	for _, part := range []string{prefix, layer, suffix} {
		if offset < int64(len(part)) {
			if offset < 0 {
				return -einval
			}

			return extent(part[offset:])
		}

		offset -= int64(len(part))
	}

	if offset > 0 {
		return -einval
	}

	return 0
}

//go:wasmexport fs_dirent_name
func fsDirentName(index uint32, entry uint32) int64 {
	dir, errno := directory(index)

	if errno != 0 {
		return errno
	}

	if entry >= dir.size {
		return 0
	}

	return extent(direntName(dir.offset + direntSize*entry))
}

//go:wasmexport fs_dirent_inode
func fsDirentInode(index uint32, entry uint32) int32 {
	dir, errno := directory(index)

	if errno != 0 {
		return int32(errno)
	}

	if entry >= dir.size {
		return -enoent
	}

	return int32(word(dir.offset + direntSize*entry + 8))
}

func main() {
}
