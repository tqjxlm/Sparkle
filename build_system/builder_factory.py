"""
Factory for creating framework builders.
"""

from build_system.builder_interface import FrameworkBuilder


def create_builder(framework: str) -> FrameworkBuilder:
    """
    Create a builder instance for the specified framework.
    
    Args:
        framework: The framework name ("glfw", "macos", "ios", "android")
        
    Returns:
        A FrameworkBuilder instance for the specified framework
        
    Raises:
        ValueError: If the framework is not supported
    """
    if framework == "glfw":
        from build_system.glfw.build import GlfwBuilder
        return GlfwBuilder()
    elif framework == "macos":
        from build_system.macos.build import MacosBuilder
        return MacosBuilder()
    elif framework == "ios":
        from build_system.ios.build import IosBuilder
        return IosBuilder()
    elif framework == "android":
        from build_system.android.build import AndroidBuilder
        return AndroidBuilder()
    else:
        raise ValueError(f"Unsupported framework: {framework}")


def get_supported_frameworks() -> list:
    """Get a list of supported framework names."""
    return ["glfw", "macos", "ios", "android"]