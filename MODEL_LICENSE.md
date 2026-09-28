# PureByte Model License 1.0

This license covers the **model weights** of the PureByte specialists (`pii`, `secrets-code` and `secrets-bin`): the
`.gguf` files as distributed by the Licensor through its official distribution channels. These are the GitHub
releases of https://github.com/purebyte-ai/purebyte, whose URLs and SHA-256 checksums are listed in
`models/models.json`, and their mirror at https://huggingface.co/purebyte, which holds the same files with the same
checksums and this license. The model cards and the checksums are documentation, licensed under the Apache License
2.0 with the rest of that repository. This license does **not** cover the PureByte runtime (engine, CLI, HTTP server,
Python bindings, specification, reference implementation, integrations and documentation) nor the training code,
recipes and data scripts (published at https://github.com/purebyte-ai/purebyte-train), which are licensed separately
under the Apache License 2.0 (see the `LICENSE` file of each repository).

In short: you may use the Model for free, for any purpose including commercial use, on hardware you own or control,
including cloud instances and continuous-integration runners that run on your behalf; you may not redistribute it,
build models from its weights, or offer it to others as a hosted service whose main value is the Model itself. What
the Model outputs is yours, and so is any model you train yourself with the open training code.

## 1. Definitions

- **"Licensor"**: Pablo Sirvent Jiménez, the author and owner of the Model.
- **"Model"**: the model weights (the `.gguf` files) released by the Licensor under this license, in any version,
  including copies obtained from its official distribution channels.
- **"Outputs"**: the decisions, scores, spans, findings, redacted copies and other results that the Model produces
  when run on data you provide.
- **"Runtime"**: the PureByte runtime or any other software that executes the Model.
- **"You"**: the individual or legal entity exercising rights under this license, and its affiliates.
- **"Derivative Model"**: any model created by modifying the Model, or by training, fine-tuning, distilling, merging,
  pruning or otherwise deriving a model from the Model's weights. A model trained on Outputs, as Section 2 allows, is
  not a Derivative Model.

## 2. Your data, the Outputs and your own models

The Licensor claims no rights in the data you process with the Model or in the Outputs, and you may use the Outputs
for any purpose, including as training data for other models. The Model runs on your machines; nothing in this
license requires you to send data, Outputs or usage information to the Licensor.

A model that you train yourself with the PureByte training code and your own copy of the data, without using the
Model's weights, is not a Derivative Model and is not covered by this license: it is yours, under the terms of the
training code and of the data you used.

## 3. Grant

Subject to the terms of this license, the Licensor grants you a worldwide, royalty-free, non-exclusive,
non-transferable, non-sublicensable license to download, install, run and use the Model, for any purpose, including
commercial purposes, on hardware you own or control, including cloud instances and continuous-integration runners
(for example GitHub-hosted runners or GitLab shared runners) that run on your behalf.

## 4. Restrictions

You may not:

1. distribute, publish, sell, rent or otherwise make the Model available to third parties, in whole or in part,
   modified or not, except by pointing to the Licensor's official distribution channels;
2. create, train, distribute or use a Derivative Model;
3. offer the Model to third parties as a hosted or managed model service whose main value is the Model itself;
4. remove, alter or obscure the license terms or notices distributed with the Model;
5. use the Model in violation of applicable law, including data protection law.

## 5. Internal copies

You may make copies of the Model as needed to use it within your organization, including copies on developer
machines, on cloud instances and continuous-integration runners that run on your behalf (for example GitHub-hosted
runners or GitLab shared runners), in their caches, in build systems and in private container images, provided that
those copies are not made available to third parties.

## 6. Evaluation and publication of results

You may benchmark the Model and publish the results, provided that you describe the version, the data and the
conditions of the measurement.

## 7. Notices and trademarks

You must keep this license and the Licensor's copyright notices with every copy of the Model. This license grants no
right to use the names, logos or trademarks of the Licensor, except to identify the origin of the Model in a factual
way.

## 8. No warranty

THE MODEL IS PROVIDED "AS IS", WITHOUT WARRANTIES OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, ACCURACY AND NON-INFRINGEMENT. IN PARTICULAR, THE
LICENSOR DOES NOT WARRANT THAT THE MODEL DETECTS ALL CREDENTIALS, PERSONAL DATA OR OTHER CONTENT IT IS DESIGNED TO
DETECT, OR THAT ITS OUTPUTS ARE FREE OF ERRORS. THE MODEL IS NOT A SUBSTITUTE FOR SECURE DEVELOPMENT PRACTICES,
CREDENTIAL ROTATION, DATA PROTECTION COMPLIANCE OR HUMAN REVIEW.

## 9. Limitation of liability

TO THE MAXIMUM EXTENT PERMITTED BY LAW, IN NO EVENT WILL THE LICENSOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, CONSEQUENTIAL OR EXEMPLARY DAMAGES ARISING FROM THE USE OF, OR INABILITY TO USE, THE MODEL OR ITS OUTPUTS,
INCLUDING UNDETECTED CREDENTIALS OR DATA, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGES. Nothing in this
license limits liability that cannot be limited under applicable law.

## 10. Termination

This license terminates automatically if you breach it. It is reinstated if you cure the breach within 30 days of
becoming aware of it. On termination you must stop using and delete all copies of the Model. Sections 2, 8, 9 and 11
survive termination.

## 11. Governing law

This license is governed by the laws of Spain, except where mandatory law provides otherwise.

## 12. Versions of this license

The Licensor may publish new versions of this license. Each release of the Model is governed by the version of the
license distributed with it.

## Contact

Questions about this license, and licenses on other terms (for example to redistribute the Model inside a product or
to offer it as a service): **pablo@purebyte.ai**.
