import 'dart:io';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';

/// 一份日志 / 文本文件的只读视图。
///
/// 多处共用：本机 `app_*` / `crash_*` 日志、从码表抓回来的 coredump / diag，
/// 以及缓存页的文本预览 —— 读取、截断、空态处理完全一样，没必要各写一遍。
///
/// 方向可配：日志排查时有用的大多在最末（崩溃信息、`coredump` 追加的 RAM 环
/// 快照都在结尾）所以要尾部；而 JSON 之类的结构化文件开头才是重点，所以要头部。
class LogTextView extends StatefulWidget {
  /// 创建文本视图。
  const LogTextView({
    super.key,
    required this.file,
    this.maxChars = 200000,
    this.keepTail = true,
  });

  /// 要显示的文件。
  final File file;

  /// 最多渲染多少字符；超出按 [keepTail] 截一端。
  final int maxChars;

  /// 超出上限时保留尾部（true，日志）还是头部（false，结构化文本）。
  final bool keepTail;

  @override
  State<LogTextView> createState() => _LogTextViewState();
}

class _LogTextViewState extends State<LogTextView> {
  String? _text;
  bool _truncated = false;
  bool _gone = false;
  String? _error;

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void didUpdateWidget(LogTextView old) {
    super.didUpdateWidget(old);
    if (old.file.path != widget.file.path) {
      _text = null;
      _truncated = false;
      _gone = false;
      _error = null;
      _load();
    }
  }

  Future<void> _load() async {
    try {
      final f = widget.file;
      if (!await f.exists()) {
        if (!mounted) return;
        setState(() => _gone = true);
        return;
      }
      final raw = await f.readAsString();
      final cut = raw.length > widget.maxChars;
      if (!mounted) return;
      setState(() {
        _truncated = cut;
        if (!cut) {
          _text = raw;
        } else if (widget.keepTail) {
          _text = raw.substring(raw.length - widget.maxChars);
        } else {
          _text = raw.substring(0, widget.maxChars);
        }
      });
    } catch (e) {
      if (!mounted) return;
      setState(() => _error = '$e');
    }
  }

  Widget _center(String text) => Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Text(
            text,
            textAlign: TextAlign.center,
            style: const TextStyle(color: AppTheme.muted),
          ),
        ),
      );

  @override
  Widget build(BuildContext context) {
    if (_gone) return _center(LocaleKeys.logFileGone.tr);
    if (_error != null) {
      return _center('${LocaleKeys.failed.tr}\n$_error');
    }
    final text = _text;
    if (text == null) {
      return const Center(child: CircularProgressIndicator());
    }
    if (text.trim().isEmpty) {
      return _center(LocaleKeys.logPreviewEmpty.tr);
    }
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (_truncated)
            Padding(
              padding: const EdgeInsets.only(bottom: 8),
              child: Text(
                widget.keepTail
                    ? LocaleKeys.logPreviewTruncated.tr
                    : LocaleKeys.logPreviewTruncatedHead.tr,
                style: const TextStyle(color: AppTheme.muted, fontSize: 12),
              ),
            ),
          SelectableText(
            text,
            style: const TextStyle(
              fontFamily: 'monospace',
              fontSize: 12,
              height: 1.35,
            ),
          ),
        ],
      ),
    );
  }
}
