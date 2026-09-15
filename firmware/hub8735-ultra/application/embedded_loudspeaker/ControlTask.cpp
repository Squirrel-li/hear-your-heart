#include "controlTask.h"


void controlTask(void* parameter) {
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}