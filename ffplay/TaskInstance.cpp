#include "TaskInstance.h"

void TaskInstance::PushAsyncTask(std::function<void()> func, uint64_t key)
{
	ST_TaskInfo info;
	info.key = key;
	info.func = func;

	std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);
	m_vTaskList.push_back(info);
}

bool TaskInstance::TaskEmpty()
{
	std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);
	return m_vTaskList.empty();
}

void TaskInstance::RunAllTask()
{
	std::vector<TaskInstance::ST_TaskInfo> temTasks;

	{
		std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);
		temTasks.swap(m_vTaskList);
	}

	for (auto &item : temTasks)
		item.func();
}

void TaskInstance::ClearAllTask()
{
	std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);
	m_vTaskList.clear();
}

void TaskInstance::RemoveTask(uint64_t key)
{
	std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);

	auto itr = m_vTaskList.begin();
	for (; itr != m_vTaskList.end(); ++itr) {
		if (itr->key == key) {
			itr = m_vTaskList.erase(itr);
			continue;
		}
	}
}
