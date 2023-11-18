#include "task-instance.h"

void task_instance::PushAsyncTask(std::function<void()> func, uint64_t key)
{
	ST_TaskInfo info;
	info.key = key;
	info.func = func;

	std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);
	m_vTaskList.push_back(info);
}

bool task_instance::TaskEmpty()
{
	std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);
	return m_vTaskList.empty();
}

void task_instance::RunAllTask()
{
	std::vector<task_instance::ST_TaskInfo> temTasks;

	{
		std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);
		temTasks.swap(m_vTaskList);
	}

	for (auto &item : temTasks)
		item.func();
}

void task_instance::ClearAllTask()
{
	std::lock_guard<std::recursive_mutex> autoLock(m_lockTask);
	m_vTaskList.clear();
}

void task_instance::RemoveTask(uint64_t key)
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
