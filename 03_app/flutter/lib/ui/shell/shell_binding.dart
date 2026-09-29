import 'package:get/get.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';

/// 产品壳 Binding。
class ShellBinding extends Bindings {
  @override
  void dependencies() {
    Get.lazyPut(ShellController.new, fenix: true);
  }
}
