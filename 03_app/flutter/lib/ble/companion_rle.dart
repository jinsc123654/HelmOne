import 'dart:math' as math;
import 'dart:typed_data';

/// BLE FS 传输用 RLE + 255 字节窗口 LZ。与固件 `companion_rle.c` 一致。
///
/// 仅用于 txt/gpx 等文本；落盘仍是原始字节，CRC/续传偏移也按未压缩计。
class CompanionRle {
  static const win = 255;
  static const look = 192;
  static const maxLit = 128;
  static const maxRun = 65;
  static const maxCpy = 66;

  static const _textExt = {
    '.txt',
    '.gpx',
    '.xml',
    '.csv',
    '.tsv',
    '.json',
    '.nmea',
    '.log',
    '.kml',
    '.md',
    '.html',
    '.htm',
  };

  /// 这类后缀值得压；压完还更大就仍走明文。
  static bool shouldCompress(String path) {
    final slash = path.lastIndexOf('/');
    final name = slash >= 0 ? path.substring(slash + 1) : path;
    final dot = name.lastIndexOf('.');
    if (dot <= 0) return false;
    return _textExt.contains(name.substring(dot).toLowerCase());
  }

  static Uint8List encode(List<int> src) {
    final e = _Enc();
    final out = BytesBuilder(copy: false);
    var off = 0;
    final tmp = Uint8List(256);
    while (off < src.length || e.lookN > 0) {
      final room = e.room;
      if (room > 0 && off < src.length) {
        final n = math.min(room, src.length - off);
        e.push(src, off, n);
        off += n;
      }
      final finish = off >= src.length;
      final n = e.pull(tmp, finish);
      if (n == 0) {
        if (finish && e.lookN == 0) break;
        continue;
      }
      out.add(tmp.sublist(0, n));
    }
    return out.takeBytes();
  }

  static Uint8List decode(List<int> src) {
    final d = _Dec();
    final out = BytesBuilder(copy: false);
    var i = 0;
    void emit(int b) {
      out.addByte(b);
      d.pushHist(b);
    }

    while (i < src.length || d.st == _Dec.stRun || d.st == _Dec.stCpy) {
      if (d.st == _Dec.stCtrl) {
        if (i >= src.length) {
          if (!d.idle) {
            throw const FormatException('RLE 流截断');
          }
          return out.takeBytes();
        }
        final c = src[i++] & 0xff;
        if (c < 0x80) {
          d.need = c + 1;
          d.st = _Dec.stLit;
        } else if (c < 0xc0) {
          d.need = (c & 0x3f) + 2;
          d.st = _Dec.stRunByte;
        } else {
          d.need = (c & 0x3f) + 3;
          d.st = _Dec.stCpyOff;
        }
      } else if (d.st == _Dec.stLit) {
        if (i >= src.length) {
          throw const FormatException('RLE 字面量截断');
        }
        emit(src[i++] & 0xff);
        d.need--;
        if (d.need == 0) d.st = _Dec.stCtrl;
      } else if (d.st == _Dec.stRunByte) {
        if (i >= src.length) {
          throw const FormatException('RLE 重复截断');
        }
        d.arg = src[i++] & 0xff;
        d.st = _Dec.stRun;
      } else if (d.st == _Dec.stRun) {
        while (d.need > 0) {
          emit(d.arg);
          d.need--;
        }
        d.st = _Dec.stCtrl;
      } else if (d.st == _Dec.stCpyOff) {
        if (i >= src.length) {
          throw const FormatException('RLE 回看截断');
        }
        d.arg = src[i++] & 0xff;
        if (d.arg == 0) {
          throw const FormatException('RLE 回看偏移为 0');
        }
        d.st = _Dec.stCpy;
      } else if (d.st == _Dec.stCpy) {
        if (d.arg > d.histN) {
          throw const FormatException('RLE 回看超出窗口');
        }
        while (d.need > 0) {
          emit(d.histAt(d.arg));
          d.need--;
        }
        d.st = _Dec.stCtrl;
      } else {
        throw const FormatException('RLE 状态错误');
      }
    }
    if (!d.idle) {
      throw const FormatException('RLE 流未结束');
    }
    return out.takeBytes();
  }
}

class _Dec {
  static const stCtrl = 0;
  static const stLit = 1;
  static const stRunByte = 2;
  static const stRun = 3;
  static const stCpyOff = 4;
  static const stCpy = 5;

  final hist = Uint8List(256);
  int histPos = 0;
  int histN = 0;
  int st = stCtrl;
  int need = 0;
  int arg = 0;

  bool get idle => st == stCtrl && need == 0;

  void pushHist(int b) {
    hist[histPos] = b;
    histPos = (histPos + 1) & 255;
    if (histN < CompanionRle.win) histN++;
  }

  int histAt(int off) => hist[(histPos - off) & 255];
}

class _Enc {
  final hist = Uint8List(256);
  int histPos = 0;
  int histN = 0;
  final look = Uint8List(CompanionRle.look);
  int lookN = 0;

  int get room => CompanionRle.look - lookN;

  void push(List<int> src, int off, int n) {
    look.setRange(lookN, lookN + n, src, off);
    lookN += n;
  }

  void _histPush(int b) {
    hist[histPos] = b;
    histPos = (histPos + 1) & 255;
    if (histN < CompanionRle.win) histN++;
  }

  int _histAt(int off) => hist[(histPos - off) & 255];

  void _consume(int n) {
    for (var i = 0; i < n; i++) {
      _histPush(look[i]);
    }
    lookN -= n;
    if (lookN > 0) {
      look.setRange(0, lookN, look, n);
    }
  }

  int _windowAt(int start, int off) {
    if (off <= start) return look[start - off];
    return _histAt(off - start);
  }

  int _runFrom(int start) {
    final avail = lookN - start;
    var n = 1;
    final max = math.min(avail, CompanionRle.maxRun);
    while (n < max && look[start + n] == look[start]) {
      n++;
    }
    return n;
  }

  (int len, int off) _matchFrom(int start) {
    final avail = lookN - start;
    var histTotal = histN + start;
    if (histTotal > CompanionRle.win) histTotal = CompanionRle.win;
    if (avail < 3 || histTotal == 0) return (0, 0);
    final maxLen = math.min(avail, CompanionRle.maxCpy);
    var bestLen = 0;
    var bestOff = 0;
    for (var off = 1; off <= histTotal; off++) {
      var m = 0;
      while (m < maxLen &&
          _windowAt(start, off - (m % off)) == look[start + m]) {
        m++;
      }
      if (m > bestLen) {
        bestLen = m;
        bestOff = off;
        if (m == maxLen) break;
      }
    }
    return (bestLen, bestOff);
  }

  int pull(Uint8List out, bool finish) {
    var used = 0;
    final outMax = out.length;
    while (lookN > 0) {
      if (!finish && lookN < CompanionRle.maxCpy) break;
      final run = _runFrom(0);
      final (ml, off) = _matchFrom(0);
      if (ml >= 3 && ml >= run) {
        if (outMax - used < 2) break;
        out[used++] = 0xc0 | (ml - 3);
        out[used++] = off;
        _consume(ml);
      } else if (run >= 3) {
        if (outMax - used < 2) break;
        out[used++] = 0x80 | (run - 2);
        out[used++] = look[0];
        _consume(run);
      } else {
        if (outMax - used < 2) break;
        var maxLit = CompanionRle.maxLit;
        if (maxLit > lookN) maxLit = lookN;
        if (maxLit > outMax - used - 1) maxLit = outMax - used - 1;
        if (maxLit < 1) break;
        var lit = 1;
        while (lit < maxLit) {
          if (!finish && lookN - lit < CompanionRle.maxCpy) break;
          final r2 = _runFrom(lit);
          final (m2, _) = _matchFrom(lit);
          if ((m2 >= 3 && m2 >= r2) || r2 >= 3) break;
          lit++;
        }
        out[used++] = lit - 1;
        out.setRange(used, used + lit, look);
        used += lit;
        _consume(lit);
      }
    }
    return used;
  }
}
