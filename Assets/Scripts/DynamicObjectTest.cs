using UnityEngine;

/// <summary>
/// 动态场景验证脚本：挂在任意收集过的 GameObject 上（如 teapot/shaderball），
/// 播放后驱动物体做刚体运动，验证 native 侧每帧 TLAS 重建是否正确跟随。
///
/// 两种运动模式可同时启用（叠加）：
///   - 旋转：绕指定轴 360 度持续旋转，速度可配（度/秒）
///   - 往返移动：沿指定轴在 ±distance 范围内来回移动，距离/速度可配
///
/// 注意：直接在 Inspector 里拖动物体会触发编辑器数据反射导致卡顿，
/// 本脚本用运行时代码驱动，避开该问题。
/// </summary>
public class DynamicObjectTest : MonoBehaviour
{
    public enum Axis { X, Y, Z }

    [Header("旋转（360 度持续）")]
    [Tooltip("启用绕轴 360 度持续旋转")]
    public bool rotateEnabled = true;

    [Tooltip("旋转轴")]
    public Axis rotationAxis = Axis.Y;

    [Tooltip("旋转速度（度/秒），360 = 每秒一圈")]
    [Range(-720f, 720f)] public float rotationSpeed = 90f;

    [Header("往返移动")]
    [Tooltip("启用沿轴往返移动")]
    public bool moveEnabled = false;

    [Tooltip("移动轴")]
    public Axis moveAxis = Axis.X;

    [Tooltip("单侧往返距离（米），物体在 ±distance 之间来回")]
    [Range(0f, 20f)] public float moveDistance = 2f;

    [Tooltip("移动速度（米/秒）")]
    [Range(0f, 10f)] public float moveSpeed = 1f;

    [Tooltip("移动曲线平滑度（1=线性往返，>1 越大端点停留越久）")]
    [Range(1f, 4f)] public float moveSmoothing = 1f;

    // 原始状态（Awake 时记录，Disable 时恢复，避免编辑器中残留位移）
    private Vector3 _initialPosition;
    private Quaternion _initialRotation;
    private float _movePhase;

    private void Awake()
    {
        _initialPosition = transform.localPosition;
        _initialRotation = transform.localRotation;
    }

    private void OnEnable()
    {
        // 从场景加载/重新启用时重置到初始状态，保证运动从已知的相位开始
        _movePhase = 0f;
    }

    private void OnDisable()
    {
        // 恢复初始 transform，避免停止播放后编辑器里物体停在半路
        if (Application.isPlaying)
        {
            transform.localPosition = _initialPosition;
            transform.localRotation = _initialRotation;
        }
    }

    private void Update()
    {
        float dt = Time.deltaTime;

        if (rotateEnabled && !Mathf.Approximately(rotationSpeed, 0f))
        {
            Vector3 axis = rotationAxis switch
            {
                Axis.X => Vector3.right,
                Axis.Y => Vector3.up,
                _      => Vector3.forward
            };
            transform.Rotate(axis, rotationSpeed * dt, Space.Self);
        }

        if (moveEnabled && moveDistance > 0f && moveSpeed > 0f)
        {
            // 相位推进：一个完整周期 = 4*distance / speed
            float period = 4f * moveDistance / moveSpeed;
            _movePhase = Mathf.Repeat(_movePhase + dt / period, 1f);

            // 三角波（0→1→0），smooth > 1 时端点减速
            float tri = _movePhase < 0.5f ? _movePhase * 2f : 2f - _movePhase * 2f;
            if (moveSmoothing > 1f)
                tri = Mathf.Pow(tri, moveSmoothing);

            float offset = (tri * 2f - 1f) * moveDistance; // ±distance
            Vector3 axis = moveAxis switch
            {
                Axis.X => Vector3.right,
                Axis.Y => Vector3.up,
                _      => Vector3.forward
            };

            // 沿初始位置 + 轴向偏移（用世界轴，配合旋转叠加更直观）
            transform.localPosition = _initialPosition + axis * offset;
        }
    }
}
