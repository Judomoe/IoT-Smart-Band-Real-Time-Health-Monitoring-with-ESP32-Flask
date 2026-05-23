from flask import Flask, render_template, request, redirect, url_for, flash
import csv
import os
import threading
import paho.mqtt.client as mqtt
import json
from datetime import datetime

app = Flask(__name__)
app.secret_key = 'secret'

SIGNUP_FILE = 'signup.csv'
MQTT_BROKER = 'broker.hivemq.com'
MQTT_PORT = 1883
MQTT_SUB_TOPIC = 'watch/ids'
MQTT_ACK_TOPIC = 'watch/acks'

registered_watch_ids = set()

# Ensure signup.csv exists and load existing watchIds
if not os.path.exists(SIGNUP_FILE):
    with open(SIGNUP_FILE, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['signupId', 'watchId', 'userName', 'Password', 'Age', 'Weight', 'Height', 'Gender'])
else:
    with open(SIGNUP_FILE, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            registered_watch_ids.add(row['watchId'])

def generate_signup_id():
    existing_ids = set()
    with open(SIGNUP_FILE, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            existing_ids.add(int(row['signupId']))
    return max(existing_ids, default=1000) + 1

@app.route('/')
def home():
    return redirect(url_for('signup'))

@app.route('/signup', methods=['GET', 'POST'])
def signup():
    if request.method == 'POST':
        watchId = request.form['watchId']
        userName = request.form['userName']
        password = request.form['password']
        age = request.form['age']
        weight = request.form['weight']
        height = request.form['height']
        gender = request.form['gender']

        with open(SIGNUP_FILE, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                if row['userName'] == userName:
                    flash('Username already exists. Choose another one.')
                    return redirect(url_for('signup'))

        signupId = generate_signup_id()

        with open(SIGNUP_FILE, 'a', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([signupId, watchId, userName, password, age, weight, height, gender])

        registered_watch_ids.add(watchId)

        # Publish acknowledgment over MQTT
        def publish_ack():
            temp_client = mqtt.Client()
            temp_client.connect(MQTT_BROKER, MQTT_PORT, 60)
            temp_client.loop_start()
            ack_msg = f"Ok {watchId}"
            temp_client.publish(MQTT_ACK_TOPIC, ack_msg)
            print(f"[MQTT] Sent signup ACK: {ack_msg}")
            temp_client.loop_stop()
            temp_client.disconnect()

        threading.Thread(target=publish_ack).start()

        flash('Signup successful. Please login.')
        return redirect(url_for('login'))

    return render_template('signup.html')

@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        userName = request.form['userName']
        password = request.form['password']

        with open(SIGNUP_FILE, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                if row['userName'] == userName and row['Password'] == password:
                    signupId = row['signupId']
                    return redirect(url_for('dashboard', signupId=signupId))

        flash('Invalid username or password.')
        return redirect(url_for('login'))

    return render_template('login.html')

@app.route('/dashboard/<signupId>')
def dashboard(signupId):
    watchId = None
    with open(SIGNUP_FILE, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['signupId'] == signupId:
                watchId = row['watchId']
                break

    if not watchId:
        return "Invalid signup ID", 404

    file_path = f'{watchId}.csv'
    if not os.path.exists(file_path):
        return f"No data file found for watch ID {watchId}", 404

    with open(file_path, newline='') as f:
        reader = csv.reader(f)
        data = list(reader)

    return render_template('dashboard.html', signupId=signupId, data=data)

@app.route('/graphs/<signupId>')
def graphs(signupId):
    watchId = None
    with open(SIGNUP_FILE, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['signupId'] == signupId:
                watchId = row['watchId']
                break

    if not watchId:
        return "Invalid signup ID", 404

    file_path = f'{watchId}.csv'
    if not os.path.exists(file_path):
        return f"No data file found for watch ID {watchId}", 404

    timestamps, steps, hr, temp, oxygen = [], [], [], [], []
    with open(file_path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            timestamps.append(row['Timestamp'])
            steps.append(row['Steps'])
            hr.append(row['HeartRate'])
            temp.append(row['Temperature'])
            oxygen.append(row['Oxygen'])

    return render_template('graphs.html',
                           signupId=signupId,
                           timestamps=timestamps,
                           steps=steps,
                           hr=hr,
                           temp=temp,
                           oxygen=oxygen)

@app.route('/fitness/<signupId>')
def fitness(signupId):
    return render_template('fitness.html', signupId=signupId)



# MQTT Callbacks
def on_connect(client, userdata, flags, rc):
    print(f"Connected to MQTT Broker with result code {rc}")
    client.subscribe(MQTT_SUB_TOPIC)
    client.subscribe("esp32/bruh/data")

def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode().strip()

    if topic == MQTT_SUB_TOPIC:
        print(f"[MQTT] Received ID: {payload}")
        if payload in registered_watch_ids:
            ack_msg = f"Ok {payload}"
            print(f"[MQTT] Sending ACK: {ack_msg}")
            client.publish(MQTT_ACK_TOPIC, ack_msg)

    elif topic == "esp32/bruh/data":
        print(f"[MQTT] Received data: {payload}")
        try:
            data = json.loads(payload)
            watch_id = data['id']

            if watch_id not in registered_watch_ids:
                print(f"[WARNING] Ignoring data from unregistered watch ID: {watch_id}")
                return

            file_path = f'{watch_id}.csv'
            new_row = [datetime.utcfromtimestamp(data['time']).strftime('%Y-%m-%d %H:%M:%S'),
                       data['steps'], data['hr'], data['temp'], data['oxygen']]
            file_exists = os.path.exists(file_path)

            with open(file_path, 'a', newline='') as f:
                writer = csv.writer(f)
                if not file_exists or os.stat(file_path).st_size == 0:
                    writer.writerow(['Timestamp', 'Steps', 'HeartRate', 'Temperature', 'Oxygen'])
                writer.writerow(new_row)

        except Exception as e:
            print(f"[ERROR] Failed to handle data message: {e}")


def mqtt_thread():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_forever()

threading.Thread(target=mqtt_thread, daemon=True).start()

if __name__ == '__main__':
    app.run(debug=True)
