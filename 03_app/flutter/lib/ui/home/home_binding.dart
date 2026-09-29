import 'package:get/get.dart';
import 'package:sifli_companion/ui/home/home_controller.dart';

/// 首页路由 Binding：懒加载 [HomeController]。
class HomeBinding extends Bindings {
  @override
  void dependencies() {
    Get.lazyPut(HomeController.new);
  }
}
