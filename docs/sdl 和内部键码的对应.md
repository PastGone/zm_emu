sdl 和内部建码值的对应关系
设置表格如下：

| 电脑键               | 功能            | 内部枚举               | 内部值 |
| -------------------- | --------------- | ---------------------- | ------ |
| `0`                  | 数字 0          | `ZMAEE_KEY_0`          | `0`    |
| `1`                  | 数字 1          | `ZMAEE_KEY_1`          | `1`    |
| `2`                  | 数字 2          | `ZMAEE_KEY_2`          | `2`    |
| `3`                  | 数字 3          | `ZMAEE_KEY_3`          | `3`    |
| `4`                  | 数字 4          | `ZMAEE_KEY_4`          | `4`    |
| `5`                  | 数字 5          | `ZMAEE_KEY_5`          | `5`    |
| `6`                  | 数字 6          | `ZMAEE_KEY_6`          | `6`    |
| `7`                  | 数字 7          | `ZMAEE_KEY_7`          | `7`    |
| `8`                  | 数字 8          | `ZMAEE_KEY_8`          | `8`    |
| `9`                  | 数字 9          | `ZMAEE_KEY_9`          | `9`    |
| `W` / `w`            | 上              | `ZMAEE_KEY_DPAD_UP`    | `13`   |
| `S` / `s`            | 下              | `ZMAEE_KEY_DPAD_DOWN`  | `14`   |
| `A` / `a`            | 左              | `ZMAEE_KEY_DPAD_LEFT`  | `15`   |
| `D` / `d`            | 右              | `ZMAEE_KEY_DPAD_RIGHT` | `16`   |
| `Q` / `q`            | 左确认 / 左软键 | `ZMAEE_KEY_SOFT_LEFT`  | `10`   |
| `E` / `e`            | 右返回 / 右软键 | `ZMAEE_KEY_SOFT_RIGHT` | `11`   |
| `Z` / `z`            | 拨号键          | `ZMAEE_KEY_CALL`       | `17`   |
| `N` / `n`            | `*` 键          | `ZMAEE_KEY_STAR`       | `20`   |
| `M` / `m`            | `#` 键          | `ZMAEE_KEY_POUND`      | `21`   |
| `空格` / `\r` / `\n` | 中心 / 确认     | `ZMAEE_KEY_CENTER`     | `25`   |
| `H` / `h`            | Home            | `ZMAEE_KEY_HOME`       | `30`   |
| `F` / `f`            | Search          | `ZMAEE_KEY_SEARCH`     | `31`   |
| `C` / `c`            | 挂机键          | `ZMAEE_KEY_DECALL`     | 待定   |
| 其他                 | 未定义          | 无                     | `0`    |

目前挂机键从没有挖掘出来
我不知道真实的环境里面有没有挂机键起作用
这个不知道