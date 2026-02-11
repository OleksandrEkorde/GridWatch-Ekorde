import json
import asyncio
from telethon import TelegramClient, events
import google.generativeai as genai

# --- НАСТРОЙКИ (Вставь свои данные) ---
API_ID = 33837936        # С my.telegram.org
API_HASH = "7ad6e2e2a28f8f6efd228c66c293c195"  # С my.telegram.org
CHANNEL = "cherkasyoblenergo" # Юзернейм канала
MY_QUEUE = "2.1"          # Твоя очередь
GEMINI_KEY = "AIzaSyB9RseZjLmINyP5tmqMAgPLpPO9xxpXSIg"
OUTPUT_FILE = "/root/GridWatchEkorde/light.json" # Куда сохранять файл для ESP32

# Настройка AI
genai.configure(api_key=GEMINI_KEY)
model = genai.GenerativeModel('gemini-1.5-flash')

# Настройка Клиента Телеграм
client = TelegramClient('session_name', API_ID, API_HASH)

async def ask_gemini(text):
    prompt = f"""
    Проанализируй сообщение от Облэнерго. Найди график для очереди {MY_QUEUE}.
    Текст: "{text}"
    
    Верни ТОЛЬКО JSON:
    {{
      "status": "off" (если выключают) или "on",
      "start": "HH:MM" (время начала откл),
      "end": "HH:MM" (время конца),
      "message": "короткий текст для экрана"
    }}
    """
    try:
        response = model.generate_content(prompt)
        # Чистим ответ от возможных ```json ... ```
        clean_json = response.text.replace('```json', '').replace('```', '').strip()
        return json.loads(clean_json)
    except Exception as e:
        print(f"Ошибка AI: {e}")
        return None

@client.on(events.NewMessage(chats=CHANNEL))
async def handler(event):
    print(f"Новое сообщение в канале!")
    text = event.message.message
    
    if not text:
        print("Сообщение без текста (фото?), пропускаем.")
        return

    # Спрашиваем Gemini
    data = await ask_gemini(text)
    
    if data:
        print(f"Результат: {data}")
        # Записываем в файл
        with open(OUTPUT_FILE, 'w') as f:
            json.dump(data, f)
            print("Файл обновлен!")

async def main():
    print("Запуск бота...")
    await client.start() # Тут попросит телефон и код при первом запуске
    print("Бот работает и слушает канал!")
    await client.run_until_disconnected()

if __name__ == '__main__':
    asyncio.run(main())
