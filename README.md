<div align="center">

# CodeBlueHalo.ai
**A Non-Contact Hostel & Lab Medical Emergency Sentinel**

<p align="center">
  <img src="https://img.shields.io/badge/Top%205-HACK--O--HERTZ%20'26-FFD700?style=for-the-badge&logo=hackaday" />
  <img src="https://img.shields.io/badge/Version-v2.0-blue?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Status-Active-brightgreen?style=for-the-badge" />
</p>

*Built during the 36-hour National Level Hackathon organized by IETE & IEEE Student Branches at New Prince Shri Bhavani College of Engineering and Technology, Chennai.*

</div>

---

## Overview

**CodeBlueHalo.ai** is an advanced, AI-powered campus safety and health-tech infrastructure system designed to protect college students with smart, real-time monitoring and instant alerts. 

By eliminating the need for wearables, we adopt a **"Software-Heavy, Hardware-Light"** architectural approach. Our system continuously monitors environmental factors and behavioral patterns in hostels and laboratories to proactively detect anomalies, alert authorities, and save lives in critical time windows.

## Key Features

- **Multi-Threat Detection Engine:** Identifies up to **7 distinct emergency classes**, ranging from medical emergencies to potential environmental hazards.
- **Edge & Cloud Synergy:** 
  - **Edge:** Utilizes lightweight **Temporal Convolutional Network Autoencoders (TCN-AE)** for instantaneous local anomaly detection (Zero-Loophole Escalation).
  - **Cloud:** Employs a robust **Transformer-based Multi-Sensor Fusion Engine** for deep relational analysis on the backend.
- **Behavioral Drift Analysis:** Flags prolonged unusual passive states (e.g., severe lethargy) as potential mental health or silent physical health deteriorations.
- **Privacy-First ML:** Integrated **Federated Learning** ensures models learn from decentralized data points without compromising individual student privacy.
- **Explainable AI Dashboard:** Real-time, transparent visualizations of sensor data, threat levels, and alert histories for administrative oversight.

## Technology Stack

| Domain | Technologies Used |
| :--- | :--- |
| **Frontend** | React 19, Vite, TypeScript, Tailwind CSS, Framer Motion, Recharts |
| **Backend** | Python, FastAPI, WebSockets |
| **Machine Learning** | PyTorch, Hugging Face Transformers, TCN-AE, Scikit-Learn |
| **Hardware / IoT** | Custom Embedded Systems (Sensors, Edge Processing Nodes) |
| **Deployment** | Docker, ngrok (Tunneling) |

## Getting Started

Follow these steps to set up the project locally.

### Prerequisites
- Node.js (v18+)
- Python (3.10+)

### 1. Clone the Repository
```bash
git clone https://github.com/CodyRohith7/CodeBlueHalo.git
cd CodeBlueHalo
```

### 2. Frontend Setup
```bash
cd frontend
npm install
npm run dev
```
*The frontend will launch on `http://localhost:5173`.*

### 3. Backend Setup
```bash
cd ../backend
python -m venv venv
source venv/bin/activate  # On Windows use `venv\Scripts\activate`
pip install -r requirements.txt
uvicorn main:app --reload
```
*The backend API will run on `http://localhost:8000`.*

## The Hackathon Journey

**HACK-O-HERTZ '26** was a high-energy, sleepless 36-hour sprint. From pitching the big idea to building hardware prototypes and delivering a 100% hardware+software integrated final demo, the journey was incredibly rewarding.

- **Phase I:** Pitching the core problem statement & solution.
- **Phase II:** Extensive coding, model tuning, and hardware prototyping.
- **Phase III:** Demoing our MVP with live real-world sensor testing.
- **Phase IV:** Final Boss Round—flawless hardware & software integration pitch.

We laughed, we coded through the night, and ultimately brought home a **Motivation Prize of ₹1000**, proudly securing a spot in the **Top 5 out of 100+ amazing teams!**

## The Team

Huge shoutout to the rockstar developers and engineers who made this happen:
- **Rohith G** (Team Leader)
- **Koushik C**
- **Naren Bhargav V S**
- **Manoj R**

*We conquered new tools, executed under crunch time, and proved that technology mixed with ambition can create real-world impact!*

<div align="center">
  <br/>
  <b>#HealthTech #EmbeddedSystems #AIForGood #ZeroLoophole</b>
</div>
