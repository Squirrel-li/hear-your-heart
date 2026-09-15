#include "UITask.h"

void UITask(void* parameter) {
	// 任务逻辑
	while (true) {
		vTaskDelay(100 / portTICK_PERIOD_MS); // 延时以降低CPU占用
	}
}