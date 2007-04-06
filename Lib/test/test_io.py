"""Unit tests for io.py."""

import unittest
from test import test_support
from itertools import chain
import io


class MockIO(io.RawIOBase):

    def __init__(self, readStack=()):
        self._readStack = list(readStack)
        self._writeStack = []

    def read(self, n=None):
        try:
            return self._readStack.pop(0)
        except:
            return b""

    def write(self, b):
        self._writeStack.append(b)
        return len(b)

    def writable(self):
        return True

    def fileno(self):
        return 42

    def readable(self):
        return True

    def seekable(self):
        return True

    def seek(self, pos, whence):
        pass

    def tell(self):
        return 42


class MockFileIO(io.BytesIO):

    def __init__(self, data):
        self.read_history = []
        io.BytesIO.__init__(self, data)

    def read(self, n=None):
        res = io.BytesIO.read(self, n)
        self.read_history.append(None if res is None else len(res))
        return res


class MockNonBlockWriterIO(io.RawIOBase):

    def __init__(self, blockingScript):
        self.bs = list(blockingScript)
        self._write_stack = []

    def write(self, b):
        self._write_stack.append(b)
        n = self.bs.pop(0)
        if (n < 0):
            raise io.BlockingIO(0, "test blocking", -n)
        else:
            return n

    def writable(self):
        return True


class IOTest(unittest.TestCase):

    def tearDown(self):
        test_support.unlink(test_support.TESTFN)

    def write_ops(self, f):
        f.write(b"blah.")
        f.seek(0)
        f.write(b"Hello.")
        self.assertEqual(f.tell(), 6)
        f.seek(-1, 1)
        self.assertEqual(f.tell(), 5)
        f.write(" world\n\n\n")
        f.seek(0)
        f.write("h")
        f.seek(-2, 2)
        f.truncate()

    def read_ops(self, f):
        data = f.read(5)
        self.assertEqual(data, b"hello")
        n = f.readinto(data)
        self.assertEqual(n, 5)
        self.assertEqual(data, b" worl")
        n = f.readinto(data)
        self.assertEqual(n, 2)
        self.assertEqual(len(data), 5)
        self.assertEqual(data[:2], b"d\n")
        f.seek(0)
        self.assertEqual(f.read(20), b"hello world\n")
        f.seek(-6, 2)
        self.assertEqual(f.read(5), b"world")
        f.seek(-6, 1)
        self.assertEqual(f.read(5), b" worl")
        self.assertEqual(f.tell(), 10)

    def test_raw_file_io(self):
        f = io.open(test_support.TESTFN, "wb", buffering=0)
        self.assertEqual(f.readable(), False)
        self.assertEqual(f.writable(), True)
        self.assertEqual(f.seekable(), True)
        self.write_ops(f)
        f.close()
        f = io.open(test_support.TESTFN, "rb", buffering=0)
        self.assertEqual(f.readable(), True)
        self.assertEqual(f.writable(), False)
        self.assertEqual(f.seekable(), True)
        self.read_ops(f)
        f.close()

    def test_raw_bytes_io(self):
        f = io.BytesIO()
        self.write_ops(f)
        data = f.getvalue()
        self.assertEqual(data, b"hello world\n")
        f = io.BytesIO(data)
        self.read_ops(f)

    def test_fileio_FileIO(self):
        import _fileio
        f = _fileio._FileIO(test_support.TESTFN, "w")
        self.assertEqual(f.readable(), False)
        self.assertEqual(f.writable(), True)
        self.assertEqual(f.seekable(), True)
        self.write_ops(f)
        f.close()
        f = _fileio._FileIO(test_support.TESTFN, "r")
        self.assertEqual(f.readable(), True)
        self.assertEqual(f.writable(), False)
        self.assertEqual(f.seekable(), True)
        self.read_ops(f)
        f.close()

    def test_PyFileIO(self):
        f = io._PyFileIO(test_support.TESTFN, "w")
        self.assertEqual(f.readable(), False)
        self.assertEqual(f.writable(), True)
        self.assertEqual(f.seekable(), True)
        self.write_ops(f)
        f.close()
        f = io._PyFileIO(test_support.TESTFN, "r")
        self.assertEqual(f.readable(), True)
        self.assertEqual(f.writable(), False)
        self.assertEqual(f.seekable(), True)
        self.read_ops(f)
        f.close()


class MemorySeekTest(unittest.TestCase):

    def testInit(self):
        buf = self.buftype("1234567890")
        bytesIo = self.ioclass(buf)

    def testRead(self):
        buf = self.buftype("1234567890")
        bytesIo = self.ioclass(buf)

        self.assertEquals(buf[:1], bytesIo.read(1))
        self.assertEquals(buf[1:5], bytesIo.read(4))
        self.assertEquals(buf[5:], bytesIo.read(900))
        self.assertEquals(self.EOF, bytesIo.read())

    def testReadNoArgs(self):
        buf = self.buftype("1234567890")
        bytesIo = self.ioclass(buf)

        self.assertEquals(buf, bytesIo.read())
        self.assertEquals(self.EOF, bytesIo.read())

    def testSeek(self):
        buf = self.buftype("1234567890")
        bytesIo = self.ioclass(buf)

        bytesIo.read(5)
        bytesIo.seek(0)
        self.assertEquals(buf, bytesIo.read())

        bytesIo.seek(3)
        self.assertEquals(buf[3:], bytesIo.read())

    def testTell(self):
        buf = self.buftype("1234567890")
        bytesIo = self.ioclass(buf)

        self.assertEquals(0, bytesIo.tell())
        bytesIo.seek(5)
        self.assertEquals(5, bytesIo.tell())
        bytesIo.seek(10000)
        self.assertEquals(10000, bytesIo.tell())


class BytesIOTest(MemorySeekTest):
    buftype = bytes
    ioclass = io.BytesIO
    EOF = b""


class StringIOTest(MemorySeekTest):
    buftype = str
    ioclass = io.StringIO
    EOF = ""


class BufferedReaderTest(unittest.TestCase):

    def testRead(self):
        rawIo = MockIO((b"abc", b"d", b"efg"))
        bufIo = io.BufferedReader(rawIo)

        self.assertEquals(b"abcdef", bufIo.read(6))

    def testBuffering(self):
        data = b"abcdefghi"
        dlen = len(data)

        tests = [
            [ 100, [ 3, 1, 4, 8 ], [ dlen, 0 ] ],
            [ 100, [ 3, 3, 3],     [ dlen ]    ],
            [   4, [ 1, 2, 4, 2 ], [ 4, 4, 1 ] ],
        ]

        for bufsize, buf_read_sizes, raw_read_sizes in tests:
            rawIo = MockFileIO(data)
            bufIo = io.BufferedReader(rawIo, buffer_size=bufsize)
            pos = 0
            for nbytes in buf_read_sizes:
                self.assertEquals(bufIo.read(nbytes), data[pos:pos+nbytes])
                pos += nbytes
            self.assertEquals(rawIo.read_history, raw_read_sizes)

    def testReadNonBlocking(self):
        # Inject some None's in there to simulate EWOULDBLOCK
        rawIo = MockIO((b"abc", b"d", None, b"efg", None, None))
        bufIo = io.BufferedReader(rawIo)

        self.assertEquals(b"abcd", bufIo.read(6))
        self.assertEquals(b"e", bufIo.read(1))
        self.assertEquals(b"fg", bufIo.read())
        self.assert_(None is bufIo.read())
        self.assertEquals(b"", bufIo.read())

    def testReadToEof(self):
        rawIo = MockIO((b"abc", b"d", b"efg"))
        bufIo = io.BufferedReader(rawIo)

        self.assertEquals(b"abcdefg", bufIo.read(9000))

    def testReadNoArgs(self):
        rawIo = MockIO((b"abc", b"d", b"efg"))
        bufIo = io.BufferedReader(rawIo)

        self.assertEquals(b"abcdefg", bufIo.read())

    def testFileno(self):
        rawIo = MockIO((b"abc", b"d", b"efg"))
        bufIo = io.BufferedReader(rawIo)

        self.assertEquals(42, bufIo.fileno())

    def testFilenoNoFileno(self):
        # XXX will we always have fileno() function? If so, kill
        # this test. Else, write it.
        pass


class BufferedWriterTest(unittest.TestCase):

    def testWrite(self):
        # Write to the buffered IO but don't overflow the buffer.
        writer = MockIO()
        bufIo = io.BufferedWriter(writer, 8)

        bufIo.write(b"abc")

        self.assertFalse(writer._writeStack)

    def testWriteOverflow(self):
        writer = MockIO()
        bufIo = io.BufferedWriter(writer, 8)

        bufIo.write(b"abc")
        bufIo.write(b"defghijkl")

        self.assertEquals(b"abcdefghijkl", writer._writeStack[0])

    def testWriteNonBlocking(self):
        raw = MockNonBlockWriterIO((9, 2, 22, -6, 10, 12, 12))
        bufIo = io.BufferedWriter(raw, 8, 16)

        bufIo.write(b"asdf")
        bufIo.write(b"asdfa")
        self.assertEquals(b"asdfasdfa", raw._write_stack[0])

        bufIo.write(b"asdfasdfasdf")
        self.assertEquals(b"asdfasdfasdf", raw._write_stack[1])
        bufIo.write(b"asdfasdfasdf")
        self.assertEquals(b"dfasdfasdf", raw._write_stack[2])
        self.assertEquals(b"asdfasdfasdf", raw._write_stack[3])

        bufIo.write(b"asdfasdfasdf")

        # XXX I don't like this test. It relies too heavily on how the
        # algorithm actually works, which we might change. Refactor
        # later.

    def testFileno(self):
        rawIo = MockIO((b"abc", b"d", b"efg"))
        bufIo = io.BufferedWriter(rawIo)

        self.assertEquals(42, bufIo.fileno())

    def testFlush(self):
        writer = MockIO()
        bufIo = io.BufferedWriter(writer, 8)

        bufIo.write(b"abc")
        bufIo.flush()

        self.assertEquals(b"abc", writer._writeStack[0])


class BufferedRWPairTest(unittest.TestCase):

    def testRWPair(self):
        r = MockIO(())
        w = MockIO()
        pair = io.BufferedRWPair(r, w)

        # XXX need implementation


class BufferedRandomTest(unittest.TestCase):

    def testReadAndWrite(self):
        raw = MockIO((b"asdf", b"ghjk"))
        rw = io.BufferedRandom(raw, 8, 12)

        self.assertEqual(b"as", rw.read(2))
        rw.write(b"ddd")
        rw.write(b"eee")
        self.assertFalse(raw._writeStack) # Buffer writes
        self.assertEqual(b"ghjk", rw.read()) # This read forces write flush
        self.assertEquals(b"dddeee", raw._writeStack[0])

    def testSeekAndTell(self):
        raw = io.BytesIO(b"asdfghjkl")
        rw = io.BufferedRandom(raw)

        self.assertEquals(b"as", rw.read(2))
        self.assertEquals(2, rw.tell())
        rw.seek(0, 0)
        self.assertEquals(b"asdf", rw.read(4))

        rw.write(b"asdf")
        rw.seek(0, 0)
        self.assertEquals(b"asdfasdfl", rw.read())
        self.assertEquals(9, rw.tell())
        rw.seek(-4, 2)
        self.assertEquals(5, rw.tell())
        rw.seek(2, 1)
        self.assertEquals(7, rw.tell())
        self.assertEquals(b"fl", rw.read(11))


class TextIOWrapperTest(unittest.TestCase):
    def testNewlines(self):
        input_lines = [ "unix\n", "windows\r\n", "os9\r", "last\n", "nonl" ]

        tests = [
            [ None, [ 'unix\n', 'windows\n', 'os9\n', 'last\n', 'nonl' ] ],
            [ '\n', input_lines ],
            [ '\r\n', input_lines ],
        ]

        encodings = ('utf-8', 'bz2')

        # Try a range of pad sizes to test the case where \r is the last
        # character in TextIOWrapper._pending_line.
        for encoding in encodings:
            for do_reads in (False, True):
                for padlen in chain(range(10), range(50, 60)):
                    pad = '.' * padlen
                    data_lines = [ pad + line for line in input_lines ]
                    # XXX: str.encode() should return bytes
                    data = bytes(''.join(data_lines).encode(encoding))

                    for newline, exp_line_ends in tests:
                        exp_lines = [ pad + line for line in exp_line_ends ]
                        bufIo = io.BufferedReader(io.BytesIO(data))
                        textIo = io.TextIOWrapper(bufIo, newline=newline,
                                                  encoding=encoding)
                        if do_reads:
                            got_lines = []
                            while True:
                                c2 = textIo.read(2)
                                if c2 == '':
                                    break
                                self.assertEquals(len(c2), 2)
                                got_lines.append(c2 + textIo.readline())
                        else:
                            got_lines = list(textIo)

                        for got_line, exp_line in zip(got_lines, exp_lines):
                            self.assertEquals(got_line, exp_line)
                        self.assertEquals(len(got_lines), len(exp_lines))

# XXX Tests for open()

def test_main():
    test_support.run_unittest(IOTest, BytesIOTest, StringIOTest,
                              BufferedReaderTest,
                              BufferedWriterTest, BufferedRWPairTest,
                              BufferedRandomTest, TextIOWrapperTest)

if __name__ == "__main__":
    test_main()
