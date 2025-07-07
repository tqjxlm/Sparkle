"""
Abstract base class for build system framework builders.
Provides a unified interface for all framework builders using polymorphism.
"""

from abc import ABC, abstractmethod
from typing import Dict, Any


class FrameworkBuilder(ABC):
    """Abstract base class for framework builders."""

    def __init__(self, framework_name: str):
        self.framework_name = framework_name

    def get_framework_name(self) -> str:
        """Get the name of the framework."""
        return self.framework_name

    @abstractmethod
    def configure_for_clangd(self, args: Dict[str, Any]) -> None:
        """Configure the build system for clangd language server support."""
        pass

    @abstractmethod
    def generate_project(self, args: Dict[str, Any]) -> None:
        """Generate IDE project files (e.g., Xcode, Visual Studio)."""
        pass

    @abstractmethod
    def build(self, args: Dict[str, Any]) -> None:
        """Build the project. Returns path to build products."""
        pass

    @abstractmethod
    def archive(self, args: Dict[str, Any]) -> str:
        """Archive the built project."""
        pass

    @abstractmethod
    def run(self, args: Dict[str, Any]) -> None:
        """Run the built project."""
        pass
