#version 460
#extension GL_EXT_ray_query : require

// Debug depth visualization shader
// Traces one ray per pixel against the TLAS, writes hit distance as grayscale
// directly to a VkImage (Unity RenderTexture) via imageStore — no CPU readback.

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, rgba8) writeonly uniform image2D outputImg;

// Camera UBO — std140 layout, row-major matrices (matching Unity's Matrix4x4 storage)
layout(set = 0, binding = 2, std140, row_major) uniform CameraUBO {
    mat4 viewInv;        // camera-to-world (inverse view matrix)
    mat4 projInv;        // inverse projection matrix
    vec4 posFar;         // xyz = camera world position, w = far clip plane
    vec2 resolution;     // xy = render width, height
    float nearPlane;
    float pad;
} cam;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    uint width  = uint(cam.resolution.x);
    uint height = uint(cam.resolution.y);
    if (pixel.x >= width || pixel.y >= height) return;

    // Generate camera ray: pixel → NDC → clip space → view space → world space
    vec2 uv = (vec2(pixel) + 0.5) / vec2(width, height);
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for screen-space (Vulkan NDC has Y down, but we want Y up)

    // Unproject: NDC → clip → view → world
    vec4 target = cam.projInv * vec4(ndc, 1.0, 1.0);
    vec3 direction = normalize((cam.viewInv * vec4(target.xyz, 0.0)).xyz);
    vec3 origin = cam.posFar.xyz;

    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas,
                          gl_RayFlagsTerminateOnFirstHitEXT,
                          0xFF,
                          origin, cam.nearPlane,
                          direction, cam.posFar.w);

    while (rayQueryProceedEXT(rq)) {
    }

    float gray;
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
        float t = rayQueryGetIntersectionTEXT(rq, true);
        gray = clamp(1.0 - t / cam.posFar.w, 0.0, 1.0);
    } else {
        gray = 0.0;  // Miss → black
    }

    // Flip Y: Vulkan imageStore y=0 is top, but Unity RenderTexture displays bottom-up
    imageStore(outputImg, ivec2(pixel.x, int(height) - 1 - int(pixel.y)), vec4(gray, gray, gray, 1.0));
}
