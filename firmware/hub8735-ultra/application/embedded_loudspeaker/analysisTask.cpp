#include "analysisTask.h"

void analysisTask(void* parameter) {
	PpgSensorElement ppgElement;
	GSensorElement gSensorElement;

	for (;;) {
		if (xQueueReceive(ppgSensorQueue, &ppgElement, 0) == pdPASS) {
			DEBUG_PRINTLN("analysisTask: Received PPG sensor data");
			float HRmax = userInfo.HRmax();
			float HRpercentage = (ppgElement.heartRateBpm / HRmax) * 100.0f;
			DEBUG_PRINT("Heart Rate: ");
			DEBUG_PRINT(ppgElement.heartRateBpm);
			DEBUG_PRINT(" BPM (");
			DEBUG_PRINT(HRpercentage);
			DEBUG_PRINTLN("% of max)");

			AudioCmdElement cmd{};
			cmd.command = AudioCommandType::Play;
			if (HRpercentage < 60.0f) {
				DEBUG_PRINTLN("Activity Level: Warm-up");
				DEBUG_PRINTLN("Immersion & Relaxation");
				String filename = "music1.mp3";
				strncpy(cmd.filename, filename.c_str(), sizeof(cmd.filename) - 1);
				cmd.filename[sizeof(cmd.filename) - 1] = '\0';
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else if (HRpercentage < 70.0f) {
				DEBUG_PRINTLN("Activity Level: Fat Burn");
				DEBUG_PRINTLN("Cadence Maintenance");
				String filename = "music2.mp3";
				strncpy(cmd.filename, filename.c_str(), sizeof(cmd.filename) - 1);
				cmd.filename[sizeof(cmd.filename) - 1] = '\0';
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else if (HRpercentage < 80.0f) {
				DEBUG_PRINTLN("Activity Level: Aerobic");
				DEBUG_PRINTLN("Pacing Sync & Moderate Boost");
				String filename = "music3.mp3";
				strncpy(cmd.filename, filename.c_str(), sizeof(cmd.filename) - 1);
				cmd.filename[sizeof(cmd.filename) - 1] = '\0';
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else if (HRpercentage < 90.0f) {
				DEBUG_PRINTLN("Activity Level: Anaerobic");
				DEBUG_PRINTLN("Critical Pivot (BPM Guide: Decel or Sprint)");
				String filename = "music4.mp3";
				strncpy(cmd.filename, filename.c_str(), sizeof(cmd.filename) - 1);
				cmd.filename[sizeof(cmd.filename) - 1] = '\0';
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else {
				DEBUG_PRINTLN("Activity Level: Maximal");
				DEBUG_PRINTLN("Emergency Decel & Voice Safety Alert");
				String filename = "music5.mp3";
				strncpy(cmd.filename, filename.c_str(), sizeof(cmd.filename) - 1);
				cmd.filename[sizeof(cmd.filename) - 1] = '\0';
				xQueueSend(audioCmdQueue, &cmd, 0);
			}
		}
		if (xQueueReceive(gSensorQueue, &gSensorElement, 0) == pdPASS) {
			DEBUG_PRINTLN("analysisTask: Received G sensor data");
		}

		// Handle serial input for audio commands
		if (Serial.available()) {
			String input = Serial.readStringUntil('\n');
			input.trim();
			if (input.length() > 0) {
				DEBUG_PRINT("Received input: ");
				DEBUG_PRINTLN(input);
			}
			AudioCmdElement cmd{};
			if (input.startsWith("play ")) {
				String filename = input.substring(5);
				filename.trim();
				cmd.command = AudioCommandType::Play;
				strncpy(cmd.filename, filename.c_str(), sizeof(cmd.filename) - 1);
				cmd.filename[sizeof(cmd.filename) - 1] = '\0';
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else if (input.equalsIgnoreCase("stop")) {
				cmd.command = AudioCommandType::Stop;
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else if (input.startsWith("volume ")) {
				int volume = input.substring(7).toInt();
				if (volume < 0) volume = 0;
				if (volume > 100) volume = 100;
				cmd.command = AudioCommandType::SetVolume;
				cmd.volumePercent = static_cast<uint8_t>(volume);
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else if (input.equalsIgnoreCase("pause")) {
				cmd.command = AudioCommandType::Pause;
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else if (input.equalsIgnoreCase("resume")) {
				cmd.command = AudioCommandType::Resume;
				xQueueSend(audioCmdQueue, &cmd, 0);
			} else {
				DEBUG_PRINTLN("Unknown command");
			}
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}