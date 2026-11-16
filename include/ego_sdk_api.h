#ifndef __EGO_SDK_API_H__
#define __EGO_SDK_API_H__

#include <stdint.h>

#if defined(_WIN32)
#  define EGO_API __declspec(dllexport)
#else
#  define EGO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** 设备工作状态 */
typedef enum EgoWorkState
{
    EGO_WORK_IDLE       = 0,    /* 空闲 */
    EGO_WORK_RECORDING  = 1,    /* 录制中 */
    EGO_WORK_UPGRADING  = 2,    /* 升级中 */
	EGO_WORK_REBOOTING  = 3,    /* 重启中 */
    EGO_WORK_UNKNOWN    = 0xFF  /* 未知 */
} EgoWorkState;

/** 升级状态 */
typedef enum EgoUpgradeStatus
{
    EGO_UPG_STATUS_NORMAL          = 0,  /* 正常状态 */
    EGO_UPG_STATUS_PREPARE         = 1,  /* 升级准备 */
    EGO_UPG_STATUS_DOWNLOADING     = 2,  /* 下载中 */
    EGO_UPG_STATUS_UPGRADING       = 3,  /* 升级中 */
    EGO_UPG_STATUS_SUCCEED         = 4,  /* 升级成功 */
    EGO_UPG_STATUS_CANCELLED       = 5,  /* 取消升级 */
    EGO_UPG_STATUS_FAILED          = 6,  /* 升级失败 */
} EgoUpgradeStatus;

/** 设备基础信息 */
typedef struct EgoDeviceInfo
{
	char deviceCode[32]; /* 设备编号 */
	char version[64];	/* 软件版本 */	
    char buildDate[32];	/* 发布日期 */
    char hardware[32];	/* 硬件版本 */
	char reserve[352];
} EgoDeviceInfo;

/** 设备工作状态 */
typedef struct EgoWorkStateInfo
{
    int32_t state;       /* EgoWorkState */
    char detail[64];     /* 状态描述 */
	char reserve[60];
} EgoWorkStateInfo;

/** 设备电池信息 */
typedef struct EgoBatteryInfo
{
    char batteryLevel;   /* 电量0~100，<0 未知 */
    char charge; 		/* 设备充电状态：0-电池供电；1-充电中 */
	char reserve[62];
} EgoBatteryInfo;

/** 设备状态 */
typedef struct EgoStateInfo
{
    EgoWorkStateInfo state;
	EgoBatteryInfo battery;
	char reserve[320];
} EgoStateInfo;

/**
 * 升级失败错误码（EgoUpgradeCallback 的 result 参数）
 */
typedef enum EgoUpgradeError
{
    EGO_UPG_ERR_OK                     = 0,           /* 成功 */
    EGO_UPG_ERR_INVALID_UPGRADE_FILE   = 0x00030016,  /* 升级包校验失败 */
    EGO_UPG_ERR_WRITE_FLASH_FAILED     = 0x00030017,  /* 写 Flash 失败 */
    EGO_UPG_ERR_FAIL_ON_UPGRADE_STATE  = 0x00030018,  /* 当前状态下不允许此操作 */
    EGO_UPG_ERR_NO_PACKET_FOR_UPGRADE  = 0x00030019,  /* 升级所需内存不足 */
    EGO_UPG_ERR_UPDATER_PARAMS_INVALID = 0x0003001b,  /* 无效的升级参数 */
    EGO_UPG_ERR_UPDATER_DVTP_NOT_MATCH = 0x00030023,  /* 升级包与设备型号不匹配 */
    EGO_UPG_ERR_UNKNOWN                = -1           /* 未知错误 */
} EgoUpgradeError;

/**
 * 升级回调
 * \param status   EgoUpgradeStatus
 * \param progress 0~100，无进度语义为 -1
 * \param result   0 成功；status 为 EGO_UPG_STATUS_FAILED 时为 EgoUpgradeError 错误码
 * \param msg      附加说明，可能为 NULL
 * \param userData 注册时传入的用户指针
 */
typedef void (*EgoUpgradeCallback)(int status, int progress, int result,
                                   const char *msg, void *userData);

/**
 * 初始化 SDK（进程内只需调用一次）
 * \param timeoutMs 同步请求超时毫秒，0 用默认 15000
 * \retval 0 成功（首次连接失败不致命，后台自动重连），-1 失败
 *
 */
EGO_API int EgoUpgradeInit(uint32_t timeoutMs);

/** 反初始化：停止后台线程并关闭连接 */
EGO_API void EgoUpgradeDeinit(void);

/** 查询设备基础信息 */
EGO_API int EgoQueryDeviceInfo(EgoDeviceInfo *info);

/** 查询设备状态（能否升级由调用方按 state和battery自行判断，如仅 EGO_WORK_IDLE 可升级） */
EGO_API int EgoQueryStateInfo(EgoStateInfo *info);

/** 注册升级回调，cb 传 NULL 取消注册 */
EGO_API int EgoRegisterUpgradeCallback(EgoUpgradeCallback cb, void *userData);

/**
 * 预升级：通知设备准备升级，并告知升级包总字节数
 * \param size 升级包总字节数，>0
 * \retval 0 设备已就绪可接收数据，-1 失败
 */
EGO_API int EgoPreUpgrade(uint32_t size);

/**
 * 传输升级包数据块（配合 EgoPreUpgrade 使用）
 *
 * 先调用 EgoPreUpgrade 告知总大小，随后将升级包分块、按顺序多次调用
 * 本接口传输；设备收满总大小后自动开始校验/烧录，结果由回调异步返回。
 * \param data 本块数据
 * \param len  本块长度，0 < len <= 16KB
 * \retval 0 本块传输成功，-1 失败
 */
EGO_API int EgoExecuteUpgrade(const void *data, uint32_t len);

/** 便捷版：内部完成 PreUpgrade + 分块读取文件传输 */
EGO_API int EgoExecuteUpgradeFile(const char *filePath);

/** 取消升级（结果通过回调返回） */
EGO_API int EgoCancelUpgrade(void);

#ifdef __cplusplus
}
#endif

#endif /* __EGO_UPGRADE_API_H__ */
