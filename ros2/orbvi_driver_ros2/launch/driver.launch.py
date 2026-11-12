from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument('config', description='Absolute path to the shared orbvi.yaml'),
        Node(package='orbvi_driver_ros2', executable='orbvi_node', output='screen',
             parameters=[{'config': LaunchConfiguration('config')}]),
    ])
