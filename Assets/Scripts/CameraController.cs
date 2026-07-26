using UnityEngine;

/// <summary>
/// 基础第一人称摄像机控制器
/// WASD 移动，鼠标转向，Space 上升，Left Shift 下降
/// </summary>
[RequireComponent(typeof(Camera))]
public class CameraController : MonoBehaviour
{
    [Header("移动设置")]
    [Tooltip("移动速度（单位/秒）")]
    public float moveSpeed = 5f;

    [Tooltip("加速倍率（按住 Left Shift 时生效）")]
    public float sprintMultiplier = 2f;

    [Header("转向设置")]
    [Tooltip("鼠标灵敏度")]
    public float mouseSensitivity = 2f;

    [Header("输入设置")]
    [Tooltip("是否需要按住鼠标右键才能转向")]
    public bool requireRightClick = true;

    private float _rotationX = 0f;
    private float _rotationY = 0f;

    private void Start()
    {
        // 初始化旋转角度为当前摄像机的欧拉角
        Vector3 euler = transform.eulerAngles;
        _rotationX = euler.y;
        _rotationY = euler.x;

        // 如果不需要右键按住，则锁定并隐藏光标
        if (!requireRightClick)
        {
            Cursor.lockState = CursorLockMode.Locked;
            Cursor.visible = false;
        }
    }

    private void Update()
    {
        HandleRotation();
        HandleMovement();

        // 按 Escape 键退出光标锁定
        if (Input.GetKeyDown(KeyCode.Escape))
        {
            Cursor.lockState = CursorLockMode.None;
            Cursor.visible = true;
        }
    }

    /// <summary>
    /// 处理鼠标转向
    /// </summary>
    private void HandleRotation()
    {
        // 如果使用右键模式，点击右键时锁定光标
        if (requireRightClick)
        {
            if (Input.GetMouseButtonDown(1))
            {
                Cursor.lockState = CursorLockMode.Locked;
                Cursor.visible = false;
            }
            if (Input.GetMouseButtonUp(1))
            {
                Cursor.lockState = CursorLockMode.None;
                Cursor.visible = true;
            }

            // 只有在光标锁定时才处理转向
            if (Cursor.lockState != CursorLockMode.Locked)
                return;
        }

        float mouseX = Input.GetAxis("Mouse X") * mouseSensitivity;
        float mouseY = Input.GetAxis("Mouse Y") * mouseSensitivity;

        // 水平旋转（绕 Y 轴）
        _rotationX += mouseX;
        // 垂直旋转（绕 X 轴），限制在 -90 ~ 90 度之间防止翻转
        _rotationY -= mouseY;
        _rotationY = Mathf.Clamp(_rotationY, -90f, 90f);

        transform.rotation = Quaternion.Euler(_rotationY, _rotationX, 0f);
    }

    /// <summary>
    /// 处理键盘移动
    /// </summary>
    private void HandleMovement()
    {
        float speed = moveSpeed;

        // 按住 Shift 加速
        if (Input.GetKey(KeyCode.LeftShift))
        {
            speed *= sprintMultiplier;
        }

        // 获取输入方向（基于摄像机本地坐标系）
        Vector3 direction = Vector3.zero;

        // 前后左右（水平面移动）
        if (Input.GetKey(KeyCode.W))
            direction += transform.forward;
        if (Input.GetKey(KeyCode.S))
            direction -= transform.forward;
        if (Input.GetKey(KeyCode.D))
            direction += transform.right;
        if (Input.GetKey(KeyCode.A))
            direction -= transform.right;

        // 上下（世界空间 Y 轴）
        if (Input.GetKey(KeyCode.Space))
            direction += Vector3.up;
        if (Input.GetKey(KeyCode.LeftControl))
            direction += Vector3.down;

        // 归一化防止斜向移动过快
        if (direction.sqrMagnitude > 0.01f)
        {
            direction.Normalize();
        }

        transform.position += direction * speed * Time.deltaTime;
    }
}
