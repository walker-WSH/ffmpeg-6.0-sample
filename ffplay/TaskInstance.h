#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class TaskInstance {
public:
	TaskInstance() = default;
	virtual ~TaskInstance() { ClearAllTask(); }

	void PushAsyncTask(std::function<void()> func, uint64_t key = 0);
	bool TaskEmpty();
	void RunAllTask();
	void ClearAllTask();
	void RemoveTask(uint64_t key);

private:
	struct ST_TaskInfo {
		uint64_t key = 0;
		std::function<void()> func;
	};

	std::recursive_mutex m_lockTask;
	std::vector<ST_TaskInfo> m_vTaskList;
};
