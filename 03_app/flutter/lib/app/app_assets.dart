/// 静态资源路径常量。
abstract class AppAssets {
  /// 码表图标（应用内展示，256px）。
  static const bikeComputer = 'assets/icons/bike_computer.png';

  /// 码表矢量源（SVG）。
  static const bikeComputerSvg = 'assets/icons/bike_computer.svg';

  /// 启动器图标源图（1024px）。
  static const bikeComputerLauncher = 'assets/icons/bike_computer_1024.png';

  /// 底部导航用的单色码表字形（白色描边 + 透明底，靠 ImageIcon 染色）。
  /// Material 图标库没有"手持码表"字形，所以这一格走图片资产。
  static const bikeComputerNav = 'assets/icons/bike_computer_nav.png';

  /// 开发者头像（关于页）。
  static const developerLogo = 'assets/icons/developer_logo.png';
}
