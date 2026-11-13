using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace UEVR.Support {
    public sealed class DebugIdentity {
        public string Guid { get; set; }
        public uint Age { get; set; }
        public uint Timestamp { get; set; }
        public uint ImageSize { get; set; }
    }

    // Read-only CodeView/MSF parsing. Neither the DLL nor the PDB is loaded/executed.
    public static class Identity {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern SafeProcessHandle OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool ReadProcessMemory(SafeProcessHandle process, IntPtr address,
            byte[] bytes, UIntPtr size, out UIntPtr read);

        static byte[] Read(Stream stream, long offset, int count) {
            if (offset < 0 || count < 0 || offset > stream.Length || count > stream.Length - offset)
                throw new InvalidDataException("Read outside file");
            var bytes = new byte[count];
            stream.Position = offset;
            int used = 0;
            while (used < count) {
                int n = stream.Read(bytes, used, count - used);
                if (n == 0) throw new EndOfStreamException();
                used += n;
            }
            return bytes;
        }
        static uint U32(byte[] bytes, int offset) { return BitConverter.ToUInt32(bytes, offset); }
        static DebugIdentity CodeView(byte[] bytes) {
            if (bytes.Length < 24 || U32(bytes, 0) != 0x53445352)
                throw new InvalidDataException("RSDS CodeView identity missing");
            var guid = new byte[16];
            Array.Copy(bytes, 4, guid, 0, 16);
            var value = new Guid(guid);
            uint age = U32(bytes, 20);
            if (value == Guid.Empty || age == 0) throw new InvalidDataException("Empty CodeView identity");
            return new DebugIdentity { Guid = value.ToString("D").ToLowerInvariant(), Age = age };
        }
        static DebugIdentity Image(Func<long, int, byte[]> read, long extent, bool mapped) {
            var dos = read(0, 64);
            if (BitConverter.ToUInt16(dos, 0) != 0x5a4d) throw new InvalidDataException("Not an MZ image");
            uint nt = U32(dos, 60);
            if (nt < 64 || nt > 1024 * 1024) throw new InvalidDataException("Invalid PE header offset");
            var header = read(nt, 24);
            if (U32(header, 0) != 0x4550 || BitConverter.ToUInt16(header, 4) != 0x8664)
                throw new InvalidDataException("Expected x64 PE image");
            int count = BitConverter.ToUInt16(header, 6), optionalSize = BitConverter.ToUInt16(header, 20);
            if (count < 1 || count > 96 || optionalSize < 168 || optionalSize > 4096)
                throw new InvalidDataException("Invalid PE section/optional header bounds");
            var optional = read((long)nt + 24, optionalSize);
            if (BitConverter.ToUInt16(optional, 0) != 0x20b || U32(optional, 108) < 7)
                throw new InvalidDataException("Missing PE32+ debug directory");
            uint imageSize = U32(optional, 56), headersSize = U32(optional, 60);
            if (imageSize == 0 || (mapped && imageSize != extent))
                throw new InvalidDataException("Loaded image size changed");
            uint debugRva = U32(optional, 160), debugSize = U32(optional, 164);
            if (debugRva == 0 || debugSize == 0 || debugSize % 28 != 0 || debugSize > 28 * 64)
                throw new InvalidDataException("Missing/bounded debug directory required");
            var sections = read((long)nt + 24 + optionalSize, count * 40);
            Func<uint, uint, long> address = (rva, size) => {
                if (size == 0 || (ulong)rva + size > imageSize)
                    throw new InvalidDataException("Debug RVA outside image");
                if (mapped) return rva;
                if ((ulong)rva + size <= headersSize) return rva;
                long result = -1;
                for (int i = 0; i < count; ++i) {
                    uint start = U32(sections, i * 40 + 12), rawSize = U32(sections, i * 40 + 16);
                    if (rva >= start && (ulong)(rva - start) + size <= rawSize) {
                        if (result != -1) throw new InvalidDataException("Ambiguous debug RVA ownership");
                        result = (long)U32(sections, i * 40 + 20) + rva - start;
                    }
                }
                if (result == -1) throw new InvalidDataException("Debug directory has no raw owner");
                return result;
            };
            var directory = read(address(debugRva, debugSize), (int)debugSize);
            DebugIdentity identity = null;
            for (int i = 0; i < directory.Length; i += 28) {
                if (U32(directory, i + 12) != 2) continue;
                uint size = U32(directory, i + 16);
                if (size < 24 || size > 65536) throw new InvalidDataException("Invalid CodeView bounds");
                long offset = mapped ? address(U32(directory, i + 20), size) : U32(directory, i + 24);
                var candidate = CodeView(read(offset, (int)size));
                if (identity != null && (identity.Guid != candidate.Guid || identity.Age != candidate.Age))
                    throw new InvalidDataException("Ambiguous CodeView identities");
                identity = candidate;
            }
            if (identity == null) throw new InvalidDataException("No CodeView debug identity");
            identity.Timestamp = U32(header, 8);
            identity.ImageSize = imageSize;
            return identity;
        }
        public static DebugIdentity FromImage(string path) {
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                return Image((at, count) => Read(file, at, count), file.Length, false);
        }
        public static DebugIdentity FromProcess(int pid, long imageBase, int imageSize) {
            if (pid <= 0 || imageBase <= 0 || imageSize <= 0) throw new ArgumentOutOfRangeException();
            using (var process = OpenProcess(0x0010 | 0x1000, false, pid)) {
                if (process.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                return Image((offset, count) => {
                    if (offset < 0 || count < 0 || offset > imageSize || count > imageSize - offset ||
                        imageBase > long.MaxValue - offset) throw new InvalidDataException("Read outside loaded module");
                    var bytes = new byte[count];
                    UIntPtr used;
                    if (!ReadProcessMemory(process, new IntPtr(imageBase + offset), bytes, (UIntPtr)(uint)count, out used) ||
                        used.ToUInt64() != (ulong)count)
                        throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                    return bytes;
                }, imageSize, true);
            }
        }
        public static DebugIdentity FromPdb(string path) {
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
                var super = Read(file, 0, 56);
                byte[] signature = System.Text.Encoding.ASCII.GetBytes("Microsoft C/C++ MSF 7.00\r\n\u001aDS\0\0\0");
                for (int i = 0; i < signature.Length; ++i)
                    if (super[i] != signature[i]) throw new InvalidDataException("Expected MSF 7 PDB");
                uint blockSize = U32(super, 32), blocks = U32(super, 40), directoryBytes = U32(super, 44), map = U32(super, 52);
                if (blockSize < 512 || blockSize > 65536 || (blockSize & (blockSize - 1)) != 0 ||
                    blocks == 0 || (ulong)blocks * blockSize > (ulong)file.Length ||
                    directoryBytes < 12 || directoryBytes > 16 * 1024 * 1024 || map >= blocks)
                    throw new InvalidDataException("Invalid/bounded MSF superblock required");
                uint directoryBlocks = (directoryBytes + blockSize - 1) / blockSize;
                if ((ulong)directoryBlocks * 4 > blockSize) throw new InvalidDataException("Unsupported oversized MSF block map");
                var blockMap = Read(file, (long)map * blockSize, checked((int)directoryBlocks * 4));
                var directory = new byte[directoryBytes];
                int copied = 0;
                var seen = new HashSet<uint>();
                for (int i = 0; i < directoryBlocks; ++i) {
                    uint block = U32(blockMap, i * 4);
                    if (block >= blocks || !seen.Add(block)) throw new InvalidDataException("Invalid/duplicate MSF directory block");
                    int n = (int)Math.Min(blockSize, directoryBytes - (uint)copied);
                    Array.Copy(Read(file, (long)block * blockSize, n), 0, directory, copied, n);
                    copied += n;
                }
                uint streams = U32(directory, 0);
                if (streams < 2 || streams > 100000 || 4UL + streams * 4UL > directoryBytes)
                    throw new InvalidDataException("Invalid MSF stream table");
                uint streamZero = U32(directory, 4), streamOne = U32(directory, 8);
                if (streamOne < 28 || streamOne == uint.MaxValue) throw new InvalidDataException("Missing PDB identity stream");
                ulong skip = streamZero == uint.MaxValue ? 0 : ((ulong)streamZero + blockSize - 1) / blockSize;
                ulong list = 4UL + streams * 4UL + skip * 4;
                ulong oneBlocks = ((ulong)streamOne + blockSize - 1) / blockSize;
                if (list + oneBlocks * 4 > directoryBytes) throw new InvalidDataException("Truncated PDB identity block list");
                uint identityBlock = U32(directory, (int)list);
                if (identityBlock >= blocks) throw new InvalidDataException("PDB identity outside block count");
                var information = Read(file, (long)identityBlock * blockSize, 28);
                var guid = new byte[16];
                Array.Copy(information, 12, guid, 0, 16);
                return new DebugIdentity { Guid = new Guid(guid).ToString("D").ToLowerInvariant(), Age = U32(information, 8) };
            }
        }
        public static bool Same(DebugIdentity a, DebugIdentity b) {
            return a != null && b != null && a.Guid == b.Guid && a.Age == b.Age;
        }
    }
}
