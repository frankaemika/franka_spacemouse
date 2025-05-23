from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    operator_position_front_arg = DeclareLaunchArgument(
        'operator_position_front',
        default_value='True',  
        description='Set to True if the operator is in the front position, otherwise False'
    )

    device_path_arg = DeclareLaunchArgument(
        'device_path',
        default_value='',
        description='Device path for the SpaceMouse'
    )

    return LaunchDescription([
        operator_position_front_arg,
        device_path_arg,
        Node(
            package='spacemouse_publisher',
            executable='pyspacemouse_publisher',
            name='spacemouse_publisher',
            output='screen',
            parameters=[
                {'operator_position_front': LaunchConfiguration('operator_position_front')},
                {'device_path': LaunchConfiguration('device_path')}
            ]
        )
    ])