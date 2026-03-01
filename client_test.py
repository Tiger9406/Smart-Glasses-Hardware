import cv2
import numpy as np
import websocket
import threading
import queue
import pyaudio
import time

WS_URI = "ws://192.168.4.1/ws"
WS_FRAME_VIDEO = 0x01
WS_FRAME_AUDIO_TX = 0x02

# Queues to absorb network jitter
video_queue = queue.Queue(maxsize=10)
audio_queue = queue.Queue(maxsize=50)

# --- 1. Audio Playback Thread ---
def audio_worker():
    p = pyaudio.PyAudio()
    # 16000 Hz, 16-bit PCM, Mono
    stream = p.open(format=pyaudio.paInt16, channels=1, rate=16000, output=True)
    
    while True:
        audio_data = audio_queue.get()
        if audio_data is None: # Exit signal
            break
        try:
            stream.write(audio_data)
        except Exception as e:
            print(f"Audio playback error: {e}")
            
    stream.stop_stream()
    stream.close()
    p.terminate()

# --- 2. Fast Network Receiving Thread ---
def on_message(ws, message):
    if not message: return
    
    frame_type = message[0]
    payload = message[1:]

    if frame_type == WS_FRAME_VIDEO:
        try:
            # Drop the oldest frame if the video decoder is falling behind
            if video_queue.full():
                video_queue.get_nowait() 
            video_queue.put_nowait(payload)
        except queue.Full:
            pass
            
    elif frame_type == WS_FRAME_AUDIO_TX:
        try:
            audio_queue.put_nowait(payload)
        except queue.Full:
            pass # Drop audio chunk if playback is somehow stalling

def ws_thread_func():
    ws = websocket.WebSocketApp(WS_URI, on_message=on_message)
    # Keep attempting to reconnect if the ESP32 drops
    while True:
        print(f"Connecting to {WS_URI}...")
        ws.run_forever()
        time.sleep(2)

# --- 3. Main Thread (Video Decoding & UI) ---
if __name__ == "__main__":
    # Start background threads
    threading.Thread(target=audio_worker, daemon=True).start()
    threading.Thread(target=ws_thread_func, daemon=True).start()

    print("Streaming... Press 'q' to quit.")
    
    # OpenCV MUST run in the main thread on most operating systems
    while True:
        try:
            # Wait up to 50ms for a new video frame
            payload = video_queue.get(timeout=0.05)
            
            # Decode and display
            nparr = np.frombuffer(payload, np.uint8)
            img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            
            if img is not None:
                cv2.imshow('ESP32-CAM Live Stream', img)
                
        except queue.Empty:
            pass # No new frame yet, just keep checking for the 'q' key
            
        # UI refresh and exit condition
        if cv2.waitKey(1) & 0xFF == ord('q'):
            print("Closing...")
            break

    # Cleanup
    audio_queue.put(None) # Tell audio thread to exit
    cv2.destroyAllWindows()