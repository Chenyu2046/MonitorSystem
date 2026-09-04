# Product

## Register

product

## Users

面向需要理解 KernScope/MonitorSystem 运行链路的开发者、评审者和项目协作者。用户需要先建立全局边界，再沿采集、计算、诊断、持久化和查询链路查看真实数据形状。

## Product Purpose

这是一个基于真实仓库代码生成的架构与 workflow 阅读界面。它把 Worker、Manager、gRPC、队列、健康评分、诊断状态、MySQL 和 QueryService 组织成可点击的全局图，并让节点详情优先解释数据，而不是展示源码。

## Brand Personality

专业、清晰、可追溯。

## Anti-references

不要做成只有模块名的低密度概览，不要用脱离代码的示例数据填充图表，也不要把源码路径和实现细节当作节点点击后的主要内容。

## Design Principles

- 先给全局边界，再提供可深入的 workflow。
- 用中文解释数据流，用真实类型和字段保留工程准确性。
- 每条关系都说明它传递的消息、结果或任务。
- 不把静态架构图伪装成线上调用追踪或实时监控面板。

## Accessibility & Inclusion

关键流程和关系使用中文辅助说明；数据表允许横向阅读并在窄屏下折叠为单列；不依赖颜色区分唯一语义，并尊重减少动态效果偏好。
