#ifndef IPC_NAVIGATION_IPC_RUNTIME_H_
#define IPC_NAVIGATION_IPC_RUNTIME_H_

/*
 * Vehicle Thread在新的遥控AUTO命令生效后调用。
 * 函数只投递请求并唤醒IPC Thread，不直接访问IPC硬件。
 */
void navigation_ipc_auto_rearm_request(void);

#endif /* IPC_NAVIGATION_IPC_RUNTIME_H_ */
