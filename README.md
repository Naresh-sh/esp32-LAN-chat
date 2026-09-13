# 📡 Free LAN Chat — ESP32

A **serverless, offline chat app** that runs entirely on an ESP32. No internet, no cloud, no accounts — just power it on and chat over local WiFi. 🔌💬

---

## ✨ Features

- 💬 **Group & private chat** in real time
- 📎 **File sharing** — up to 10MB per file
- 😄 **Emoji picker** with search & categories
- ❤️ **Message reactions**
- 🗑️ **Delete messages**
- ⌨️ **Typing indicator**
- 🌙 **Dark / light theme** toggle
- 📱 **Mobile-friendly** responsive UI
- 🕒 **60-second message history** on reconnect
- 📶 **Admin-only WiFi scan & connect** to a router
- 🔐 **Password-protected login**

---

## 🧠 How It Works

- ESP32 boots into **AP + STA mode** 📶
- Devices connect to the ESP32's own WiFi hotspot
- Chat runs over **WebSockets** ⚡
- HTTP server serves the web UI + handles file uploads
- Files are stored on **LittleFS** (onboard flash) 💾

No app to install — just open a browser. 🌐

---

## 🛠️ Hardware Requirements

- ✅ Any **ESP32 Dev Board**
- ✅ USB cable
- ✅ 4MB+ flash (8MB/16MB recommended for bigger file storage)

---

## 📚 Required Libraries

Install via **Arduino Library Manager**:

- `WiFi` (built-in with ESP32 core)
- `WebServer` (built-in)
- `WebSocketsServer` — by *Links2004*
- `ArduinoJson` — v7+
- `LittleFS` (built-in with ESP32 core)

---

## ⚙️ Setup

1. 📥 Clone or download this repo
2. 📂 Open the `.ino` file in **Arduino IDE**
3. 🔧 Select board: `ESP32 Dev Module`
4. 🧩 Choose a **Partition Scheme** with enough flash for files (see below ⚠️)
5. 🔌 Connect your ESP32 via USB
6. ⬆️ Upload the sketch

---

## 🔑 Configuration

Edit these values at the top of the sketch before uploading:

```cpp
const char* AP_SSID = "FreeLANChat";
const char* AP_PASS = "password";      // min 8 chars
const char* CHAT_LOGIN_PASS = "112211";
```

🔒 Always change the default password before real use!

---

## ⚠️ Flash Storage Note

Default Arduino partition schemes only reserve a **small amount** of space for file storage.

To fully use the 10MB file-sharing limit:

- 🔧 Go to **Tools → Partition Scheme**
- 📦 Pick a scheme with a large **SPIFFS / FATFS** partition
- 💡 Recommended: boards with **8MB or 16MB** flash

---

## 🚀 Usage

1. ⚡ Power on the ESP32
2. 📶 Connect your phone/laptop to WiFi: `FreeLANChat`
3. 🌍 Open a browser and go to: `http://192.168.4.1`
4. 🔑 Enter your name + password
5. 💬 Start chatting!

---

## 📎 Sending Files

- Tap the **📎 attach button**
- Pick a file (max **10MB**)
- File uploads and appears in chat as a downloadable card ⬇️

---

## 🧑‍💼 Admin Features

The **first person to connect** automatically becomes admin 👑

Admin can:

- 📡 Scan nearby WiFi networks
- 🔗 Connect the ESP32 to a home router
- ❌ Disconnect from router

---

## 📁 Project Structure

```
📦 project-root
 ┣ 📜 esp32_lan_chat.ino     → main sketch (server + web UI)
 ┗ 📜 README.md              → this file
```

Everything — HTML, CSS, JS — is embedded **inside the sketch** for a single-file deploy. 🎯

---

## 🐞 Known Limitations

- 📴 No internet access required or used — fully offline
- 🗄️ File storage limited by flash size
- 🕒 Chat history only covers the **last 60 seconds**
- 👥 Max simultaneous clients tied to WebSocket library limit

---

## 🤝 Contributing

Pull requests are welcome! 🙌

- 🐛 Found a bug? Open an issue
- 💡 Have an idea? Suggest a feature
- 🔧 Want to improve something? Fork & PR

---

## 📄 License

MIT License — free to use, modify, and share. ✅

---

## ⭐ Support

If this project helped you, consider giving it a **star** ⭐ on GitHub!

Made with ❤️ for offline, private, serverless communication. 📴🔒
