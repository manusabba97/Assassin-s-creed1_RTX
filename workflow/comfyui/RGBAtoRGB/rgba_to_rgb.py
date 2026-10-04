class RGBAtoRGB:
    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"image": ("IMAGE",)}}

    RETURN_TYPES = ("IMAGE",)
    RETURN_NAMES = ("RGB",)
    FUNCTION = "convert"
    CATEGORY = "image"

    def convert(self, image):
        # ComfyUI IMAGE: [B, H, W, C]
        channels = image.shape[-1]
        if channels == 3:
            return (image,)
        if channels == 4:
            return (image[..., :3],)
        raise ValueError(f"RGBAtoRGB: expected 3 or 4 channels, got {channels}")


NODE_CLASS_MAPPINGS = {
    "RGBAtoRGB": RGBAtoRGB,
}

NODE_DISPLAY_NAME_MAPPINGS = {
    "RGBAtoRGB": "RGBA → RGB",
}
