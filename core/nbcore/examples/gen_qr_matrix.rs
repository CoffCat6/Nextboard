fn main() {
    let code = qrcode::QrCode::with_error_correction_level("https://nb.dev/qr", qrcode::EcLevel::L).unwrap();
    let w = code.width();
    println!("// payload=https://nb.dev/qr ECC=L version={} modules={}", (w - 17) / 4, w);
    for y in 0..w {
        let mut row = String::from("    ");
        for x in 0..w {
            row.push(if code[(x, y)] == qrcode::Color::Dark { '1' } else { '0' });
        }
        println!("    \"{}\",", row);
    }
}
