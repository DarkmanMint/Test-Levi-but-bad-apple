# BadAppleOverlay — LeviLaunchroid native mod test source

**สถานะ:** นี่เป็น source project สำหรับสร้างแพ็กทดสอบ HUD ที่ลากย้ายได้เท่านั้น ยังไม่ใช่ `.levipack` ที่คอมไพล์แล้ว และยังไม่มีระบบเล่น MP4/เสียง

## วิธี Build จากมือถือ
1. ดาวน์โหลด ZIP นี้และแตกไฟล์
2. สร้าง repository ใหม่บน GitHub แล้วอัปโหลดไฟล์ทั้งหมดจากโฟลเดอร์นี้ (รวม `.github/workflows/build.yml`)
3. เปิดแท็บ Actions แล้วเลือก workflow `Build LeviPack` จากนั้นกด Run workflow
4. หาก workflow สำเร็จ ให้เปิด run ล่าสุด แล้วดาวน์โหลด artifact `BadAppleOverlay-levipack`
5. นำ `.levipack` ที่ได้เข้า LeviLauncher เพื่อทดสอบแค่การโหลดแพ็กและการลากหน้าต่าง

## ข้อจำกัดสำคัญ
- โค้ดนี้เป็นต้นแบบทดสอบหน้าต่าง HUD เท่านั้น ไม่มี decoder MP4 และไม่มีเสียง
- ยังไม่ได้ทดสอบคอมไพล์ในสภาพแวดล้อมจริง ต้องดูผล GitHub Actions ก่อน
- ถ้า build ล้มเหลว ให้ส่ง log สีแดงของ workflow มาเพื่อแก้ต่อ
- ทดสอบกับ instance สำรอง/โลกทดสอบก่อน เพราะ native mods ทำงานใน process ของเกม

อ้างอิง API: LeviLaunchroid Native Mod Quick Start และ Mod Menu/Input API.
