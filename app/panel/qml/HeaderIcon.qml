// 全应用共享的矢量图标词汇(纯 QML 绘制,零图片资源)。
//
// 两处用法:
//  1. 头部命令按钮:`ThemedButton { iconName: "translate" }` —— 颜色由按钮按自身文字色注入,
//     因此 primary(强调底)与 ghost/secondary 三种状态下对比度都成立;
//  2. 内容类型标识:列表条目的类型徽标(EntryCard)、导航标签页(Main.qml)直接实例化,
//     颜色由调用方注入,与所在处的文字色保持一致。
//
// 设计约束(与 docs/theme.md §38 一致):统一 Outline 线性风、1.5px 描边、
// 16px 视口;只用几何图形,不用字体字形/emoji(跨字体渲染不可控)。
//
// ⚠️ 几何数字一律按 **16px 视口**写,再由 `size`/`unit` 统一缩放 ——
// 改造型请只改"16px 基准下的数字",不要针对某个尺寸单独写死(与 AppMark.qml 同一手法)。
import QtQuick
import Nextboard.panel

Item {
    id: root

    /// 图标种类:
    ///  "translate" | "settings" | "pin"  —— 头部命令按钮;
    ///  "all" | "text" | "image" | "file" | "todo" —— 内容类型(导航标签页 / 条目类型徽标),
    ///  其中 "pin" 同时兼作"固定"状态标识;
    ///  "chevron" | "pencil" | "trash"    —— 卡片右下角操作按钮(展开收起 / 编辑 / 删除),
    ///  其中 "chevron" 只画一个方向,反向由使用处 rotation: 180 翻转;
    ///  "filter"                          —— 筛选入口(漏斗),头部「筛选」按钮用;
    ///  "multiselect"                     —— 条目多选(三行勾选列表),
    ///    与 "todo" 单复选框刻意区分:todo = 一条任务,multiselect = 多条并列勾选;
    ///  "capture" | "keyboard" | "network" | "info" —— 设置页侧栏分类导航(docs/22),
    ///    与近邻图标的区分:"capture" 四角取景框(vs "pin" 图钉),"keyboard" 键位阵列
    ///    (vs "multiselect" 勾选列表),"network" 三节点网状(translate 的地球是圆 +
    ///    经线,不混淆),"info" 圆 + i(单字母立形,无近邻)。
    ///  "plus"                           —— QuickRail「添加快捷功能」占位钮,
    ///    两臂正交的圆头加号,与近邻图标无共用母题,不会混淆。
    property string kind: ""
    /// 描边与填充色(由调用方按所在处的文字色注入)
    property color color: Theme.colors.textPrimary
    /// 视口边长:默认 16(头部命令按钮);导航标签页传 14,元信息徽标传
    /// Theme.metrics.fontMicro(11,与徽标文字同号)。
    property real size: 16
    /// 线宽基准(16px 视口下)
    readonly property real stroke: 1.5

    /// 以 **16px 视口**为设计基准的缩放系数:下面的几何数字一律按 16px 写,再统一
    /// 乘本系数 —— 换尺寸时不会各处漏改,也不会出现"图标 12px 但描边还是 1.5px"的走样。
    /// (与 AppMark.qml 的 unit 同一手法。)
    readonly property real unit: size / 16

    /// kind → 形状组件(见下方 Loader;kind 由调用方在创建期给定)
    readonly property var kindComponents: {
        "text": textComp,
        "multiselect": multiselectComp,
        "capture": captureComp,
        "translate": translateComp,
        "settings": settingsComp,
        "pin": pinComp,
        "all": allComp,
        "image": imageComp,
        "file": fileComp,
        "chevron": chevronComp,
        "pencil": pencilComp,
        "trash": trashComp,
        "bell": bellComp,
        "todo": todoComp,
        "filter": filterComp,
        "keyboard": keyboardComp,
        "network": networkComp,
        "info": infoComp,
        "plus": plusComp,
    }

    implicitWidth: size
    implicitHeight: size

    // 与按钮同节奏换色(变体切换时不会突变)
    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

    // ---- 图标本体:按 kind 只实例化**一组**形状 ----
    // 此前 18 组形状全部作为兄弟 Item 常驻(靠 visible 切换),单个图标实例要
    // 构建 80~120 个 QML item(一张条目卡最多 9 个图标),列表委托因此过重、
    // 快速滚动与模型 reset 的重建代价都被放大。改为 Component + Loader 后,
    // 每个图标实例只构建被选中的那一组;kind 是创建期确定的属性,不运行时切换。
    Loader {
        anchors.fill: parent
        sourceComponent: root.kindComponents[root.kind] ?? null
    }

    // ---- 翻译:地球(语言 / 翻译的通用隐喻) ----
    // (组件化)kind = translate:只被 Loader 按需实例化
    Component {
        id: translateComp
        Item {
        anchors.fill: parent

        // 外圈
        Rectangle {
            anchors.centerIn: parent
            width: 14 * root.unit; height: 14 * root.unit; radius: width / 2
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        // 赤道
        Rectangle {
            anchors.centerIn: parent
            width: 14 * root.unit; height: root.stroke * root.unit; radius: height / 2
            color: root.color
        }
        // 经线(用胶囊形近似经线圈)
        Rectangle {
            anchors.centerIn: parent
            width: 7 * root.unit; height: 14 * root.unit; radius: width / 2
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        }
    }

    // ---- 设置:齿轮(八齿 + 中空圆环) ----
    // (组件化)kind = settings:只被 Loader 按需实例化
    Component {
        id: settingsComp
        Item {
        anchors.fill: parent

        // 八个齿:委托自身填满图标区并绕中心旋转,齿固定在顶端 → 绕中心均匀分布
        Repeater {
            model: 8
            delegate: Item {
                anchors.fill: parent
                rotation: index * 45
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 1 * root.unit
                    width: 2.5 * root.unit; height: 4 * root.unit; radius: width / 2
                    color: root.color
                }
            }
        }
        // 齿轮主体(圆环,中心留孔)
        Rectangle {
            anchors.centerIn: parent
            width: 11 * root.unit; height: 11 * root.unit; radius: width / 2
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        }
    }

    // ---- 固定:图钉(帽 + 身 + 针尖) ----
    // (组件化)kind = pin:只被 Loader 按需实例化
    Component {
        id: pinComp
        Item {
        anchors.fill: parent

        Rectangle {   // 钉帽
            anchors.horizontalCenter: parent.horizontalCenter
            y: 2 * root.unit
            width: 10 * root.unit; height: 3 * root.unit; radius: height / 2
            color: root.color
        }
        Rectangle {   // 钉身
            anchors.horizontalCenter: parent.horizontalCenter
            y: 5 * root.unit
            width: 5 * root.unit; height: 4.5 * root.unit; radius: 1 * root.unit
            color: root.color
        }
        Rectangle {   // 针尖
            anchors.horizontalCenter: parent.horizontalCenter
            y: 9.5 * root.unit
            width: 1.8 * root.unit; height: 4.5 * root.unit; radius: width / 2
            color: root.color
        }
        }
    }

    // ---- 全部:2×2 网格(收全量历史的"总览"隐喻) ----
    // 用网格而不是再画一组横线:与下面的"文本"图标必须一眼可辨,横线系会互相撞脸。
    // (组件化)kind = all:只被 Loader 按需实例化
    Component {
        id: allComp
        Item {
        anchors.fill: parent

        // 四个方块:7 + 2 间距 + 7 = 16,右下角正好落满视口
        Repeater {
            model: 4
            delegate: Rectangle {
                x: (index % 2) * 9 * root.unit
                y: Math.floor(index / 2) * 9 * root.unit
                width: 7 * root.unit; height: 7 * root.unit; radius: 1.5 * root.unit
                color: "transparent"
                border.width: root.stroke * root.unit
                border.color: root.color
            }
        }
        }
    }

    // ---- 文本:段落横线(左对齐、末行短 —— 只靠线条不画外框,
    //      免得与"文件"的文档/文件夹轮廓混淆) ----
    // (组件化)kind = text:只被 Loader 按需实例化
    Component {
        id: textComp
        Item {
        id: textIcon
        anchors.fill: parent

        /// 三条线的宽度(16px 视口下);末行短是"段落"的关键特征
        readonly property var lineWidths: [12, 12, 7.5]
        Repeater {
            model: 3
            delegate: Rectangle {
                // ⚠️ 用 id 限定(textIcon.lineWidths):只写 `parent.lineWidths` 运行期也能
                // 取到,但类型系统里 `parent` 是 Repeater 而非本 Item,qmllint 会报
                // missing-property。澄清引用既消掉误报,也避免以后有人挪动结构时取错对象。
                x: 2 * root.unit
                y: (2.5 + index * 4.5) * root.unit
                width: textIcon.lineWidths[index] * root.unit
                height: 2 * root.unit; radius: height / 2
                color: root.color
            }
        }
        }
    }

    // ---- 图片:画框 + 日 + 山(框内裁剪,山峰由旋转方块裁出) ----
    // (组件化)kind = image:只被 Loader 按需实例化
    Component {
        id: imageComp
        Item {
        anchors.fill: parent

        Rectangle {
            id: imageFrame
            anchors.centerIn: parent
            width: 14 * root.unit; height: 11.5 * root.unit; radius: 2 * root.unit
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        // 画框内部:统一裁剪,山的下半部被底边切平(与常见"图片"图标一致)
        Item {
            anchors.fill: imageFrame
            clip: true

            Rectangle {   // 日
                x: 3 * root.unit; y: 2.5 * root.unit
                width: 2.8 * root.unit; height: 2.8 * root.unit; radius: width / 2
                color: root.color
            }
            Rectangle {   // 山:旋转 45° 的方块,顶点在上、下半被裁掉
                x: 5.5 * root.unit; y: 6.5 * root.unit
                width: 7 * root.unit; height: 7 * root.unit
                rotation: 45
                transformOrigin: Item.Center
                color: root.color
            }
        }
        }
    }

    // ---- 文件:文件夹(页签 + 主体) ----
    // (组件化)kind = file:只被 Loader 按需实例化
    Component {
        id: fileComp
        Item {
        anchors.fill: parent

        // ⚠️ 两者的描边必须**落在同一像素区间**,否则交界处会叠出一条 2 倍粗的深色带。
        //    实测坑:页签占 y 2..6(底描边 4.5..6),主体原来从 y=6 起(顶描边 6..7.5),
        //    两条只在 y=6 相接 → 合计 4.5..7.5 共 3px,是其它描边(1.5px)的两倍,
        //    48px 放大后一眼可见"上沿特别厚"。
        //    修正:主体上沿 = 页签下沿 − 描边宽 = 2 + 4 − 1.5 = 4.5,
        //    于是两条描边完全重合,交界与其余描边同粗。
        Rectangle {   // 主体(下沿 14,与页签上沿共同占满竖向视口)
            x: 1 * root.unit; y: 4.5 * root.unit
            width: 14 * root.unit; height: 9.5 * root.unit; radius: 1.5 * root.unit
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        Rectangle {   // 页签:下沿 6,其底描边与主体顶描边重合(见上)
            x: 1 * root.unit; y: 2 * root.unit
            width: 7 * root.unit; height: 4 * root.unit; radius: 1.2 * root.unit
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        }
    }

    // ---- 展开收起:V 形折角(默认开口向下 = 再点收起;反向由使用处 rotation: 180 翻转) ----
    // 只造一个形:同一行里"展开/收起"是互斥状态,两个图形会显得是一对独立按钮。
    //
    // ⚠️ 定位数值是**常量**,不要写成引用自身 width 的表达式:
    //    初版 `x: 8*unit - width*SQRT1_2*0.85` 在 rotation 下触发绑定环,qml.exe 直接挂死
    //    (无报错、永不退出)。两杆的位置由 tmp 工具按"上端点必须同时落在尖点 (8,7.6)"
    //    解出(见 progress.md),数值代死即可,数学不变。
    // (组件化)kind = chevron:只被 Loader 按需实例化
    Component {
        id: chevronComp
        Item {
        anchors.fill: parent

        Rectangle {   // 左斜杆(−45°):从左下到尖点
            width: 5.5 * root.unit; height: root.stroke * root.unit; radius: height / 2
            x: 3.305 * root.unit
            y: 8.795 * root.unit
            rotation: -45
            transformOrigin: Item.Center
            color: root.color
        }
        Rectangle {   // 右斜杆(+45°):从尖点到右下
            width: 5.5 * root.unit; height: root.stroke * root.unit; radius: height / 2
            x: 7.195 * root.unit
            y: 8.795 * root.unit
            rotation: 45
            transformOrigin: Item.Center
            color: root.color
        }
        }
    }

    // ---- 编辑:铅笔(斜杆 + 笔尖) ----
    // 笔杆 = 45° 旋转胶囊;笔尖 = 45° 旋转小方块(菱形),其沿杆方向的"下尖"即笔尖,
    // "上尖"与笔杆下端相接(两段共用同一条 45° 对角线,数值由 tmp 工具解出后代死)。
    // ⚠️ 同 chevron:定位一律用常量,不要引用自身 width/height —— rotation 下自引用
    //    会触发绑定环,qml.exe 挂死且无报错。
    // (组件化)kind = pencil:只被 Loader 按需实例化
    Component {
        id: pencilComp
        Item {
        anchors.fill: parent

        Rectangle {   // 笔杆:45° 斜置的长胶囊(左上笔尾 → 右下笔尖)
            width: 7.5 * root.unit; height: 3 * root.unit; radius: height / 2
            x: 4.45 * root.unit; y: 5.7 * root.unit
            rotation: 45
            transformOrigin: Item.Center
            color: root.color
        }
        Rectangle {   // 笔尖:旋转 45° 的方块,沿杆方向的"下尖"构成笔尖
            width: 2.8 * root.unit; height: 2.8 * root.unit
            x: 10.85 * root.unit; y: 9.85 * root.unit
            rotation: 45
            transformOrigin: Item.Center
            color: root.color
        }
        }
    }

    // ---- 删除:垃圾桶(桶盖横杆 + 提手 + 桶身) ----
    // (组件化)kind = trash:只被 Loader 按需实例化
    Component {
        id: trashComp
        Item {
        anchors.fill: parent

        Rectangle {   // 提手(顶部小方块,Outline)
            anchors.horizontalCenter: parent.horizontalCenter
            y: 1.5 * root.unit
            width: 5 * root.unit; height: 2.5 * root.unit; radius: 1 * root.unit
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        Rectangle {   // 桶盖:横贯视口的横杆
            anchors.horizontalCenter: parent.horizontalCenter
            y: 5 * root.unit
            width: 12 * root.unit; height: root.stroke * root.unit; radius: height / 2
            color: root.color
        }
        Rectangle {   // 桶身:上宽下略窄的感觉由圆角给出(Outline)
            anchors.horizontalCenter: parent.horizontalCenter
            y: 7 * root.unit
            width: 9.5 * root.unit; height: 7.5 * root.unit; radius: 1.8 * root.unit
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        }
    }

    // ---- 提醒:铃铛(罩 + 底摆 + 铃舌) ----
    // 待办卡片上"已设提醒"的徽标图标(§6.3)。Outline 与其余图标同族。
    // (组件化)kind = bell:只被 Loader 按需实例化
    Component {
        id: bellComp
        Item {
        anchors.fill: parent

        Rectangle {   // 罩:上方圆顶(用大圆角近似的钟罩)
            anchors.horizontalCenter: parent.horizontalCenter
            y: 3.5 * root.unit
            width: 8 * root.unit; height: 7.5 * root.unit
            radius: 3.5 * root.unit
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        Rectangle {   // 底摆:横杆(与罩底描边相接)
            anchors.horizontalCenter: parent.horizontalCenter
            y: 11 * root.unit
            width: 10.5 * root.unit; height: root.stroke * root.unit; radius: height / 2
            color: root.color
        }
        Rectangle {   // 铃舌:底摆下方的小方块
            anchors.horizontalCenter: parent.horizontalCenter
            y: 12.6 * root.unit
            width: 1.8 * root.unit; height: 1.8 * root.unit
            radius: 0.9 * root.unit
            color: root.color
        }
        }
    }

    // ---- 待办:复选框(条目框 + 对勾) ----
    // 对勾两根短杆(左短右长)交出"已勾"的识别度;框用 Outline 与 trash 桶身同族。
    // ⚠️ 两根杆的定位是常量数值(与 chevron/pencil 同规矩):不要引用自身尺寸,
    //    rotation 下自引用会触发绑定环挂死(docs/progress.md 同类踩坑)。
    // (组件化)kind = todo:只被 Loader 按需实例化
    Component {
        id: todoComp
        Item {
        anchors.fill: parent

        Rectangle {   // 条目框
            anchors.centerIn: parent
            width: 11.5 * root.unit; height: 11.5 * root.unit; radius: 1.5 * root.unit
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        Rectangle {   // 对勾短杆(左,60° 下行)
            width: 3.2 * root.unit; height: root.stroke * root.unit; radius: height / 2
            x: 4.1 * root.unit; y: 9.4 * root.unit
            rotation: 60
            transformOrigin: Item.Center
            color: root.color
        }
        Rectangle {   // 对勾长杆(右上,-45°)
            width: 6.4 * root.unit; height: root.stroke * root.unit; radius: height / 2
            x: 6.6 * root.unit; y: 9.4 * root.unit
            rotation: -45
            transformOrigin: Item.Center
            color: root.color
        }
        }
    }

    // ---- 筛选:漏斗(上宽下窄的梯形轮廓 + 短颈) ----
    // 用"两条斜边 + 顶部横边 + 底部短颈"构成经典漏斗;不画内部斜纹
    // (16px 视口下内部细节会糊成一团,且与"文本"的横线系撞脸)。
    // ⚠️ 斜边用 rotation 摆角度,定位全用常量数值 —— 不要引用自身尺寸,
    //    rotation 下自引用会触发绑定环挂死(与 chevron/pencil/todo 同规矩)。
    // (组件化)kind = filter:只被 Loader 按需实例化
    Component {
        id: filterComp
        Item {
        anchors.fill: parent

        Rectangle {   // 顶部横边(漏斗口)
            x: 1.8 * root.unit; y: 3.2 * root.unit
            width: 12.4 * root.unit; height: root.stroke * root.unit
            radius: height / 2
            color: root.color
        }
        Rectangle {   // 左斜边(从左上收向中下)
            width: 7.6 * root.unit; height: root.stroke * root.unit
            radius: height / 2
            x: 3.5 * root.unit; y: 6.2 * root.unit
            rotation: 58
            transformOrigin: Item.Center
            color: root.color
        }
        Rectangle {   // 右斜边(镜像)
            width: 7.6 * root.unit; height: root.stroke * root.unit
            radius: height / 2
            x: 4.9 * root.unit; y: 6.2 * root.unit
            rotation: -58
            transformOrigin: Item.Center
            color: root.color
        }
        Rectangle {   // 短颈(漏斗下口)
            x: 7.1 * root.unit; y: 9.4 * root.unit
            width: 1.8 * root.unit; height: root.stroke * root.unit
            radius: height / 2
            color: root.color
        }
        }
    }

    // ---- 多选:三行"行首小方块 + 横线"的列表勾选形态 ----
    // 与近邻图标的区分线(16px 视口下都很容易撞脸,刻意避开):
    //  - vs "todo"(单个复选框+对勾):todo 是**一条**任务的勾选态,本图标是
    //    **多条并列**的勾选列表 —— 三行 × (方块+横线);
    //  - vs "text"(三条横线):text 无行首方块、末行短;本图标每行都有方块,
    //    且三行等长 —— "行首方块"是勾选列表的关键特征,横线系不与之混淆。
    // ⚠️ 方块用**实心小方块**(无描边):3.6px 尺寸配 1.5 描边在小尺寸下发糊,
    //    实心点缩到 11px 徽标尺寸也仍清晰。
    // ⚠️ 定位全用常量数值,不要引用自身尺寸(rotation 下自引用会挂绑定,
    //    与 chevron/pencil/filter 同一规矩);行距 4.5,三行均布。
    // ⚠️ 行 y 通过 Repeater 的 id 引用(multiSel.lineYs[index]):不要写
    //    parent.parent.parent —— 层级引用结构一变就断(qmllint 也会报)。
    // (组件化)kind = multiselect:只被 Loader 按需实例化
    Component {
        id: multiselectComp
        Item {
        id: multiSel
        anchors.fill: parent

        readonly property var lineYs: [2.6, 7.1, 11.6]
        Repeater {
            model: 3
            delegate: Item {
                anchors.fill: parent
                required property int index
                // 行首实心小方块(勾选框)
                Rectangle {
                    x: 1.6 * root.unit
                    y: (multiSel.lineYs[index] + 0.9) * root.unit
                    width: 3.6 * root.unit; height: 3.6 * root.unit
                    radius: 0.8 * root.unit
                    color: root.color
                }
                // 行内容横线(方块右侧起,三条等长)
                Rectangle {
                    x: 6.8 * root.unit
                    y: (multiSel.lineYs[index] + 1.7) * root.unit
                    width: 7.6 * root.unit; height: 1.8 * root.unit; radius: height / 2
                    color: root.color
                }
            }
        }
        }
    }

    // ---- 截图:四角取景框 + 对焦十字(设置页「截图」分类) ----
    // 与 "pin"(图钉)的区分:本图标是**中空**的四角括号 + 中心十字,
    // 无任何实心主体;pin 有连续实心形(帽/身/针)。
    // 四个角各自两笔(横 + 竖),角长 4.5 / 笔宽 1.8,16px 视口下留 1 边距。
    // (组件化)kind = capture:只被 Loader 按需实例化
    Component {
        id: captureComp
        Item {
        id: capture
        anchors.fill: parent

        // 四角锚点(角括号的外角),顺序:TL / TR / BL / BR
        readonly property var corners: [
            { x: 1,    y: 1 }, { x: 10.5, y: 1 },
            { x: 1,    y: 14.2 }, { x: 10.5, y: 14.2 },
        ]
        Repeater {
            model: capture.corners
            delegate: Item {
                anchors.fill: parent
                required property var modelData
                // 横笔(沿角向外角方向伸展 4.5)
                Rectangle {
                    x: modelData.x * root.unit
                    y: modelData.y * root.unit
                    width: 4.5 * root.unit; height: 1.8 * root.unit
                    radius: 0.6 * root.unit
                    color: root.color
                }
                // 竖笔(BR 角的 y 要向上反伸,用锚点 y - 笔高)
                Rectangle {
                    x: modelData.x * root.unit
                    y: (modelData.y > 8 ? modelData.y - 2.7 : modelData.y) * root.unit
                    width: 1.8 * root.unit; height: 4.5 * root.unit
                    radius: 0.6 * root.unit
                    color: root.color
                }
            }
        }
        // 对焦十字(中心,4.4 臂长)
        Rectangle {
            anchors.centerIn: parent
            width: 4.4 * root.unit; height: 1.4 * root.unit; radius: height / 2
            color: root.color
        }
        Rectangle {
            anchors.centerIn: parent
            width: 1.4 * root.unit; height: 4.4 * root.unit; radius: width / 2
            color: root.color
        }
        }
    }

    // ---- 键盘:键位阵列(设置页「快捷键」分类) ----
    // 1.5px 描边的外壳 + 实心键帽:3.6px 键帽配 1.5 描边在小尺寸发糊(同 multiselect),
    // 键帽一律实心。与 "multiselect"(三行方块+横线)的区分:本图标有**封闭边框**,
    // 键帽 3 颗 + 空格条,排布在框内。
    // (组件化)kind = keyboard:只被 Loader 按需实例化
    Component {
        id: keyboardComp
        Item {
        anchors.fill: parent

        // 键盘外壳(圆角描边)
        Rectangle {
            x: 1 * root.unit; y: 3.2 * root.unit
            width: 14 * root.unit; height: 9.6 * root.unit
            radius: 1.6 * root.unit
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        // 三颗键帽(首行)
        Repeater {
            model: [3.2, 7.0, 10.8]
            delegate: Item {
                anchors.fill: parent
                required property real modelData
                Rectangle {
                    x: modelData * root.unit; y: 5.6 * root.unit
                    width: 2.2 * root.unit; height: 2.2 * root.unit
                    radius: 0.6 * root.unit
                    color: root.color
                }
            }
        }
        // 空格条
        Rectangle {
            x: 4.6 * root.unit; y: 9.4 * root.unit
            width: 6.8 * root.unit; height: 1.8 * root.unit
            radius: 0.6 * root.unit
            color: root.color
        }
        }
    }

    // ---- 网络:三节点网状(设置页「网络与代理」分类) ----
    // 实心节点 + 细连线(TL—TR 横线 / TR—BR 竖线 / TL—BR 斜线)。
    // 与 "translate"(地球 = 圆 + 经线)的区分:本图标是**离散节点**,无封闭圆。
    // ⚠️ 斜线用旋转矩形画(同齿轮齿的 rotation 手法);旋转项里不引用自身几何。
    // (组件化)kind = network:只被 Loader 按需实例化
    Component {
        id: networkComp
        Item {
        anchors.fill: parent

        // TL→BR 斜线(旋转 45°,两端让出节点半径)
        Item {
            anchors.centerIn: parent
            width: 9.6 * root.unit; height: 1.4 * root.unit
            rotation: 45
            Rectangle { anchors.fill: parent; radius: height / 2; color: root.color }
        }
        // TL→TR 横线
        Rectangle {
            x: 4.6 * root.unit; y: 2.9 * root.unit
            width: 6.8 * root.unit; height: 1.4 * root.unit; radius: height / 2
            color: root.color
        }
        // TR→BR 竖线
        Rectangle {
            x: 11.7 * root.unit; y: 4.6 * root.unit
            width: 1.4 * root.unit; height: 6.8 * root.unit; radius: width / 2
            color: root.color
        }
        // 三个节点(TL / TR / BR,实心圆)
        Repeater {
            model: [{ x: 2.2, y: 2.2 }, { x: 11.2, y: 2.2 }, { x: 11.2, y: 11.2 }]
            delegate: Item {
                anchors.fill: parent
                required property var modelData
                Rectangle {
                    x: modelData.x * root.unit; y: modelData.y * root.unit
                    width: 2.6 * root.unit; height: 2.6 * root.unit
                    radius: width / 2
                    color: root.color
                }
            }
        }
        }
    }

    // ---- 关于:圆 + i(设置页「关于」分类) ----
    // 圆环描边 + 上点下竖的 i;与 "settings"(八齿齿轮)同用圆形母题,
    // 但齿轮有外齿、本图标是光圆环 + 内部字母形,一眼可分。
    // (组件化)kind = info:只被 Loader 按需实例化
    Component {
        id: infoComp
        Item {
        anchors.fill: parent

        Rectangle {
            anchors.centerIn: parent
            width: 13.4 * root.unit; height: 13.4 * root.unit
            radius: width / 2
            color: "transparent"
            border.width: root.stroke * root.unit
            border.color: root.color
        }
        // i 的点
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 3.4 * root.unit
            width: 1.8 * root.unit; height: 1.8 * root.unit
            radius: width / 2
            color: root.color
        }
        // i 的竖笔
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 6.6 * root.unit
            width: 1.8 * root.unit; height: 5.2 * root.unit
            radius: 0.6 * root.unit
            color: root.color
        }
        }
    }

    // ---- 添加:加号(QuickRail「添加快捷功能」占位钮) ----
    // 两枚圆头胶囊正交;16px 视口下臂长 10px,悬停可读、缩小不糊。
    // (组件化)kind = plus:只被 Loader 按需实例化
    Component {
        id: plusComp
        Item {
        anchors.fill: parent

        Rectangle {   // 横臂
            anchors.centerIn: parent
            width: 10 * root.unit; height: 1.8 * root.unit
            radius: height / 2
            color: root.color
        }
        Rectangle {   // 竖臂
            anchors.centerIn: parent
            width: 1.8 * root.unit; height: 10 * root.unit
            radius: width / 2
            color: root.color
        }
        }
    }
}
