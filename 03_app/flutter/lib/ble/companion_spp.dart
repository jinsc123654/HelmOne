import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/log/app_log.dart';

/// Classic SPP 链路阶段。
enum CompanionSppState {
  /// 未连接。
  disconnected,

  /// 正在打开 RFCOMM。
  connecting,

  /// 已连接，等设备 hello。
  waitingHello,

  /// 可收发。
  ready,

  /// 出错。
  error,
}

/// Classic RFCOMM 客户端。GATT 就绪后对 Status 里的 Classic 地址
/// `createBond(TRANSPORT_BREDR)`，再开 Serial Port。
class CompanionSppClient {
  CompanionSppClient._();

  static final CompanionSppClient instance = CompanionSppClient._();

  static const _tag = 'CompanionSpp';
  static const _method = MethodChannel('com.sifli.sifli_companion/spp');
  static const _events = EventChannel('com.sifli.sifli_companion/spp_events');

  /// 出厂 Classic 地址（eFuse MAC）。GATT 未连上时也可用来开 SPP。
  static const classicFallback = '78:11:09:9A:77:C8';

  final _stateCtrl = StreamController<CompanionSppState>.broadcast();
  final _rx = BytesBuilder(copy: false);
  final _pending = <int, Completer<CompanionSppReply>>{};

  StreamSubscription<dynamic>? _eventSub;
  CompanionSppState _state = CompanionSppState.disconnected;
  String? _address;
  String? _lastError;
  int _helloProto = 0;
  String _helloAddr = '';
  int _tid = 1;
  Completer<void>? _helloWait;
  Future<void>? _connectLock;

  CompanionSppState get state => _state;
  Stream<CompanionSppState> get stateStream => _stateCtrl.stream;
  String? get address => _address;
  String? get lastError => _lastError;
  int get helloProto => _helloProto;
  String get helloAddr => _helloAddr;
  bool get isReady => _state == CompanionSppState.ready;

  void _setState(CompanionSppState next) {
    if (_state == next) return;
    _state = next;
    _stateCtrl.add(next);
  }

  /// 用 Device Status 里的 Classic 地址打开 Serial Port。不要传假 LE 地址。
  Future<void> connect(String classicAddress) async {
    if (!Platform.isAndroid) {
      throw StateError('SPP 仅 Android');
    }
    if (_connectLock != null) {
      await _connectLock;
      if (isReady && _address == classicAddress) return;
    }
    final gate = Completer<void>();
    _connectLock = gate.future;
    try {
      await close();
      _address = classicAddress;
      _lastError = null;
      _helloProto = 0;
      _helloAddr = '';
      _setState(CompanionSppState.connecting);

      _eventSub = _events.receiveBroadcastStream().listen(
        _onEvent,
        onError: (Object e) {
          _fail('SPP 事件: $e');
        },
      );

      try {
        await _method.invokeMethod<bool>('connect', {'address': classicAddress});
      } on PlatformException catch (e) {
        await close();
        _fail(e.message ?? e.code);
        rethrow;
      }

      _setState(CompanionSppState.waitingHello);
      _helloWait = Completer<void>();
      try {
        await _helloWait!.future.timeout(const Duration(seconds: 12));
      } on TimeoutException {
        await close();
        _fail('未收到 SPP hello（RFCOMM 未真正起来）');
        throw CompanionFsException('未收到 SPP hello');
      }
      if (_state != CompanionSppState.ready) {
        final msg = _lastError ?? 'SPP hello 失败';
        await close();
        _fail(msg);
        throw CompanionFsException(msg);
      }
    } finally {
      if (!gate.isCompleted) gate.complete();
      if (_connectLock == gate.future) _connectLock = null;
    }
  }

  Future<void> close() async {
    await _eventSub?.cancel();
    _eventSub = null;
    _rx.clear();
    for (final c in _pending.values) {
      if (!c.isCompleted) {
        c.completeError(CompanionFsException('SPP 断开'));
      }
    }
    _pending.clear();
    if (_helloWait != null && !_helloWait!.isCompleted) {
      _helloWait!.complete();
    }
    _helloWait = null;
    try {
      await _method.invokeMethod<bool>('close');
    } catch (_) {}
    _setState(CompanionSppState.disconnected);
  }

  void _fail(String message) {
    _lastError = message;
    _setState(CompanionSppState.error);
    AppLog.w(_tag, message);
  }

  void _onEvent(dynamic raw) {
    if (raw is! Map) return;
    final type = raw['type'] as String?;
    if (type == 'pairing') {
      AppLog.i(_tag, '请求 Classic 配对 ${raw['address']}，请点系统弹窗');
      return;
    }
    if (type == 'closed') {
      if (_helloWait != null && !_helloWait!.isCompleted) {
        _helloWait!.complete();
      }
      if (_state == CompanionSppState.ready ||
          _state == CompanionSppState.waitingHello ||
          _state == CompanionSppState.connecting) {
        _fail('对端关闭 SPP');
      }
      return;
    }
    if (type != 'data') return;
    final bytes = raw['bytes'];
    if (bytes is! Uint8List && bytes is! List) return;
    _rx.add(bytes is Uint8List ? bytes : Uint8List.fromList(List<int>.from(bytes)));
    _drain();
  }

  void _drain() {
    final buf = _rx.takeBytes();
    var off = 0;
    while (off + 4 <= buf.length) {
      final body = buf[off] |
          (buf[off + 1] << 8) |
          (buf[off + 2] << 16) |
          (buf[off + 3] << 24);
      if (body <= 0 || body > CompanionFs.sppPayloadMax + 8) {
        _fail('SPP 帧长度异常 $body');
        return;
      }
      if (off + 4 + body > buf.length) break;
      final frame = Uint8List.fromList(buf.sublist(off + 4, off + 4 + body));
      off += 4 + body;
      _onFrame(frame);
    }
    if (off < buf.length) {
      _rx.add(buf.sublist(off));
    }
  }

  void _onFrame(Uint8List body) {
    if (body.length < 3) return;
    final reply = CompanionSppReply(
      cmd: body[0],
      tid: body[1],
      status: body[2],
      payload: body.length > 3
          ? Uint8List.fromList(body.sublist(3))
          : Uint8List(0),
    );

    if (reply.cmd == CompanionFs.sppHello) {
      if (reply.payload.length >= 8) {
        _helloProto = reply.payload[0] | (reply.payload[1] << 8);
        _helloAddr = CompanionFs.formatBdAddr(reply.payload.sublist(2, 8));
      }
      if (_state == CompanionSppState.waitingHello) {
        _setState(CompanionSppState.ready);
      }
      if (_helloWait != null && !_helloWait!.isCompleted) {
        _helloWait!.complete();
      }
      AppLog.i(_tag, 'hello proto=0x${_helloProto.toRadixString(16)} addr=$_helloAddr');
      return;
    }

    final wait = _pending.remove(reply.tid);
    if (wait == null || wait.isCompleted) {
      AppLog.w(_tag, '无等待的 SPP 回复 cmd=0x${reply.cmd.toRadixString(16)} tid=${reply.tid}');
      return;
    }
    wait.complete(reply);
  }

  Future<CompanionSppReply> _rpc(
    int cmd,
    Uint8List payload, {
    Duration timeout = const Duration(seconds: 30),
  }) async {
    if (!isReady && _state != CompanionSppState.waitingHello) {
      throw CompanionFsException('SPP 未连接');
    }
    final tid = _tid;
    _tid = _tid >= 255 ? 1 : _tid + 1;
    final bodyLen = 2 + payload.length;
    final pkt = Uint8List(4 + bodyLen);
    pkt[0] = bodyLen & 0xff;
    pkt[1] = (bodyLen >> 8) & 0xff;
    pkt[2] = (bodyLen >> 16) & 0xff;
    pkt[3] = (bodyLen >> 24) & 0xff;
    pkt[4] = cmd;
    pkt[5] = tid;
    pkt.setAll(6, payload);

    final wait = Completer<CompanionSppReply>();
    _pending[tid] = wait;
    try {
      await _method.invokeMethod<bool>('write', {'bytes': pkt});
      final reply = await wait.future.timeout(timeout);
      if (reply.status != 0) {
        throw CompanionFsException(
          'SPP cmd=0x${cmd.toRadixString(16)}',
          status: reply.status,
        );
      }
      return reply;
    } on TimeoutException {
      _pending.remove(tid);
      throw CompanionFsException('SPP 等待超时 cmd=0x${cmd.toRadixString(16)}');
    }
  }

  Uint8List _u16le(int v) => Uint8List.fromList([v & 0xff, (v >> 8) & 0xff]);

  Uint8List _u32le(int v) => Uint8List.fromList([
        v & 0xff,
        (v >> 8) & 0xff,
        (v >> 16) & 0xff,
        (v >> 24) & 0xff,
      ]);

  int _u32At(Uint8List b, [int o = 0]) =>
      b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24);

  Uint8List _pathBytes(String path) =>
      Uint8List.fromList(utf8.encode(CompanionFs.wirePath(path)));

  /// 列目录。不要 LIST 巨大的 `vmap/` / `map/`。
  Future<List<CompanionFsEntry>> fsList(String path, {int cursor = 0}) async {
    final all = <CompanionFsEntry>[];
    var cur = cursor;
    while (true) {
      final pathb = _pathBytes(path);
      final payload = Uint8List(2 + pathb.length);
      payload.setAll(0, _u16le(cur));
      payload.setAll(2, pathb);
      final reply = await _rpc(CompanionFs.list, payload);
      final page = CompanionFsEntry.parseList(reply.payload);
      if (page.isEmpty) break;
      all.addAll(page);
      cur += page.length;
      if (reply.payload.length < CompanionFs.sppPayloadMax - 64) break;
    }
    return all;
  }

  /// 查询单个路径。
  Future<CompanionFsEntry> fsStat(String path) async {
    final reply = await _rpc(CompanionFs.stat, _pathBytes(path));
    if (reply.payload.length < 9) {
      throw CompanionFsException('STAT 回复过短');
    }
    return CompanionFsEntry(
      type: reply.payload[0],
      size: _u32At(reply.payload, 1),
      mtime: _u32At(reply.payload, 5),
      name: path.split('/').where((s) => s.isNotEmpty).last,
    );
  }

  /// 下载。[offset] 为已收到的字节数。
  Future<Uint8List> fsDownload(
    String path, {
    int offset = 0,
    int? length,
  }) async {
    final int remain;
    if (length != null) {
      remain = length;
    } else {
      final st = await fsStat(path);
      remain = (st.size - offset).clamp(0, st.size);
    }

    final out = BytesBuilder(copy: false);
    var off = offset;
    var left = remain;
    while (left > 0) {
      final want = left > CompanionFs.sppPayloadMax
          ? CompanionFs.sppPayloadMax
          : left;
      final pathb = _pathBytes(path);
      final pkt = Uint8List(CompanionFs.sppReadHdrLen + pathb.length);
      pkt.setAll(0, _u32le(off));
      pkt.setAll(4, _u16le(want));
      pkt.setAll(6, pathb);
      final reply = await _rpc(CompanionFs.read, pkt);
      out.add(reply.payload);
      if (reply.payload.isEmpty) break;
      off += reply.payload.length;
      left -= reply.payload.length;
      if (reply.payload.length < want) break;
    }
    return out.takeBytes();
  }

  /// 上传。[resume] 为 true 时接着设备上未完成的 `*.part` 写。
  Future<void> fsUpload(
    String path,
    List<int> data, {
    bool resume = true,
  }) async {
    final crcAll = CompanionFs.crc32Part(data);
    final pathb = _pathBytes(path);
    final open = Uint8List(5 + pathb.length);
    open.setAll(0, _u32le(data.length));
    open[4] = resume ? CompanionFs.writeFlagResume : 0;
    open.setAll(5, pathb);

    final reply = await _rpc(CompanionFs.writeOpen, open);
    if (reply.payload.length < CompanionFs.writeOpenReplyLen) {
      throw CompanionFsException('WRITE_OPEN 回复过短');
    }
    var off = _u32At(reply.payload, 0);
    final partCrc = reply.payload.length >= 8 ? _u32At(reply.payload, 4) : 0;
    if (off > data.length) {
      throw CompanionFsException('续传偏移 $off 超出文件 ${data.length}');
    }
    if (off > 0 && !CompanionFs.resumePrefixMatches(data, off, partCrc)) {
      if (!resume) {
        throw CompanionFsException('续传前缀损坏', status: 5);
      }
      return fsUpload(path, data, resume: false);
    }

    while (off < data.length) {
      final n = (data.length - off) > CompanionFs.sppPayloadMax
          ? CompanionFs.sppPayloadMax
          : (data.length - off);
      await _rpc(
        CompanionFs.writeData,
        Uint8List.fromList(data.sublist(off, off + n)),
        timeout: const Duration(seconds: 60),
      );
      off += n;
    }

    await _rpc(CompanionFs.writeClose, _u32le(crcAll));
  }

  /// 删除文件或空目录。
  Future<void> fsDelete(String path) async {
    await _rpc(CompanionFs.delete, _pathBytes(path));
  }

  /// 创建目录。
  Future<void> fsMkdir(String path) async {
    await _rpc(CompanionFs.mkdir, _pathBytes(path));
  }

  /// 移动 / 重命名。
  Future<void> fsRename(String from, String to) async {
    final oldb = _pathBytes(from);
    final newb = _pathBytes(to);
    if (oldb.length > 255) {
      throw CompanionFsException('源路径过长');
    }
    final pkt = Uint8List(1 + oldb.length + newb.length);
    pkt[0] = oldb.length;
    pkt.setAll(1, oldb);
    pkt.setAll(1 + oldb.length, newb);
    await _rpc(CompanionFs.rename, pkt);
  }
}

/// 一帧 SPP 回复。
class CompanionSppReply {
  CompanionSppReply({
    required this.cmd,
    required this.tid,
    required this.status,
    required this.payload,
  });

  final int cmd;
  final int tid;
  final int status;
  final Uint8List payload;
}
