//! 临时诊断:打印显示器几何与整屏捕获尺寸,排查截图分辨率/多屏问题。
//! 用法:cd core && cargo run --example monitor_probe

fn main() {
    let list = nbcore::capture::monitors();
    println!("显示器数量: {}", list.len());
    for (i, m) in list.iter().enumerate() {
        println!(
            "  [{i}] name={:?} x={} y={} w={} h={} scale={} primary={}",
            m.name, m.x, m.y, m.width, m.height, m.scale_factor, m.is_primary
        );
    }
    for i in 0..list.len() as u32 {
        match nbcore::capture::capture_monitor(i) {
            Ok(img) => {
                let px = img.width as f64 * img.height as f64;
                println!(
                    "capture[{i}] = {}x{} ({} 字节, {:.1} B/px)",
                    img.width,
                    img.height,
                    img.rgba.len(),
                    img.rgba.len() as f64 / px
                );
            }
            Err(err) => println!("capture[{i}] 失败: {err}"),
        }
    }
    // 按名捕获:这是宿主实际使用的路径,单独验证一遍(含"名字对不上"的报错)
    println!("\n按设备名捕获(宿主配对路径):");
    for m in &list {
        match nbcore::capture::capture_monitor_by_name(&m.name) {
            Ok(img) => println!("  {:?} -> {}x{}", m.name, img.width, img.height),
            Err(err) => println!("  {:?} 失败: {err}", m.name),
        }
    }
    match nbcore::capture::capture_monitor_by_name(r"\\.\DISPLAY99") {
        Ok(_) => println!("  ⚠️ 不存在的设备名竟然成功(不应发生)"),
        Err(err) => println!("  不存在的设备名如期报错: {err}"),
    }
}
